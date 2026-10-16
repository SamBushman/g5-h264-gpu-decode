#!/usr/bin/env python3
"""Layer 3 of the PROMO4 simulation (issue #1): R5xx fragment shader (US) decoder + single-pixel interpreter.

Semantics and bit layouts are from AMD R5xx_Acceleration_v1.5 sec 8.3-8.4 and the US_* register tables (extracted text, 2010-06-08).
Supported: ALU and OUTPUT instructions (all RGB/alpha ops, presubtract, swizzle incl. 0/0.5/1, input/output modifiers, clamp,
write masks, temp/const/inline-constant sources, RGB_WMASK/ALPHA_WMASK, output FIFO per TARGET) and TEX (LD/PROJ/LODBIAS/LOD/DXDY via a
user callback; TEXKILL). NOT supported (raises Unsupported rather than guessing): flow control, predication, relative addressing,
MDH/MDV quad ops, TEX semaphore timing. Arithmetic is IEEE float32-rounded; real hardware is FP24 in places (see skill
ati-x1900-driver-quirks), so a match here proves the program's LOGIC, not bit-exact hardware precision.

Program format: list of tuples, ALU/OUT = (cmn, rgb_addr, alpha_addr, rgb_inst, alpha_inst, rgba_inst), TEX = (cmn, tex_inst, tex_addr).
"""
import math, struct


class Unsupported(Exception):
    pass


def f32(x):
    try:
        return struct.unpack('>f', struct.pack('>f', x))[0]
    except OverflowError:
        return math.copysign(float('inf'), x)


def bits(w, hi, lo):
    return (w >> lo) & ((1 << (hi - lo + 1)) - 1)


def inline_const(v7):
    e, m = (v7 >> 3) & 0xf, v7 & 7
    if v7 == 0: return 2.0 ** -10
    if e == 0: return (m / 8.0) * 2.0 ** -6       # denormal-ish: matches table (0x1 -> 2^-9, 0x2 -> 2^-8)
    return (1 + m / 8.0) * 2.0 ** (e - 7)


class Pixel:
    def __init__(self, consts=None, sampler=None):
        self.t = [[0.0] * 4 for _ in range(128)]
        self.c = consts or {}
        self.sampler = sampler
        self.out = {}          # target -> [r,g,b,a] last written (None where not written)
        self.killed = False


def _src(px, addr, const, rel):
    if rel: raise Unsupported('relative addressing')
    if const: return list(px.c.get(addr, [0.0] * 4))
    if addr & 0x80: v = inline_const(addr & 0x7f); return [v, v, v, v]
    return list(px.t[addr & 0x7f])


def _swz(vec, s):
    return vec[s] if s < 4 else {4: 0.0, 5: 0.5, 6: 1.0}.get(s, 0.0)


def _mod(v, m):
    return -v if m == 1 else abs(v) if m == 2 else -abs(v) if m == 3 else v


def _omod(v, om):
    return v * {0: 1, 1: 2, 2: 4, 3: 8, 4: .5, 5: .25, 6: .125, 7: 1}[om]


def _rcp(a): return math.copysign(float('inf'), a) if a == 0 else 1.0 / a


def step(px, ins):
    cmn = ins[0]; typ = bits(cmn, 1, 0)
    if typ == 2: raise Unsupported('flow control instruction')
    if bits(cmn, 5, 3): raise Unsupported('predication')
    if typ == 3: return _tex(px, ins)
    _, rgb_addr, alpha_addr, rgb_inst, alpha_inst, rgba = ins
    srcs = []
    for k in range(3):
        rs = _src(px, bits(rgb_addr, 7 + 10 * k, 10 * k), bits(rgb_addr, 8 + 10 * k, 8 + 10 * k), bits(rgb_addr, 9 + 10 * k, 9 + 10 * k))
        as_ = _src(px, bits(alpha_addr, 7 + 10 * k, 10 * k), bits(alpha_addr, 8 + 10 * k, 8 + 10 * k), bits(alpha_addr, 9 + 10 * k, 9 + 10 * k))
        srcs.append([rs[0], rs[1], rs[2], as_[3]])
    rp, ap = bits(rgb_addr, 31, 30), bits(alpha_addr, 31, 30)
    s0, s1 = srcs[0], srcs[1]
    srcp = [{0: 1 - 2 * s0[i], 1: s1[i] - s0[i], 2: s1[i] + s0[i], 3: 1 - s0[i]}[rp] for i in range(3)] + \
           [{0: 1 - 2 * s0[3], 1: s1[3] - s0[3], 2: s1[3] + s0[3], 3: 1 - s0[3]}[ap]]
    pool = srcs + [srcp]
    # RGB inputs A,B (rgb_inst), C (rgba); alpha inputs A,B (alpha_inst), C (rgba)
    def rgb_in(sel, r, g, b, m): v = pool[sel]; return [_mod(_swz(v, x), m) for x in (r, g, b)]
    A = rgb_in(bits(rgb_inst, 1, 0), bits(rgb_inst, 4, 2), bits(rgb_inst, 7, 5), bits(rgb_inst, 10, 8), bits(rgb_inst, 12, 11))
    B = rgb_in(bits(rgb_inst, 14, 13), bits(rgb_inst, 17, 15), bits(rgb_inst, 20, 18), bits(rgb_inst, 23, 21), bits(rgb_inst, 25, 24))
    C = rgb_in(bits(rgba, 13, 12), bits(rgba, 16, 14), bits(rgba, 19, 17), bits(rgba, 22, 20), bits(rgba, 24, 23))
    def al_in(sel, z, m): return _mod(_swz(pool[sel], z), m)
    aA = al_in(bits(alpha_inst, 13, 12), bits(alpha_inst, 16, 14), bits(alpha_inst, 18, 17))
    aB = al_in(bits(alpha_inst, 20, 19), bits(alpha_inst, 23, 21), bits(alpha_inst, 25, 24))
    aC = al_in(bits(rgba, 26, 25), bits(rgba, 29, 27), bits(rgba, 31, 30))
    # DP4's 4th component: the ALPHA unit's A/B inputs
    rop, aop = bits(rgba, 3, 0), bits(alpha_inst, 3, 0)
    sop = None
    if aop >= 8 and aop <= 13:
        sop = {8: lambda a: 2.0 ** a, 9: lambda a: math.log2(a) if a > 0 else float('-inf'), 10: _rcp,
               11: lambda a: _rcp(math.sqrt(a)) if a > 0 else float('inf'), 12: lambda a: math.sin(a * 2 * math.pi),
               13: lambda a: math.cos(a * 2 * math.pi)}[aop](aA)
    dp = None
    if rop == 1: dp = A[0] * B[0] + A[1] * B[1] + A[2] * B[2]
    elif rop == 2: dp = A[0] * B[0] + A[1] * B[1] + A[2] * B[2] + aA * aB
    elif rop == 3: dp = A[0] * B[0] + A[1] * B[1] + C[2]
    if rop in (11, 12) or aop in (14, 15): raise Unsupported('MDH/MDV quad op')
    if rop == 6 or rop > 12: raise Unsupported('reserved RGB op %d' % rop)
    if rop == 0: rgb = [A[i] * B[i] + C[i] for i in range(3)]
    elif rop in (1, 2, 3): rgb = [dp] * 3
    elif rop == 4: rgb = [a if a < b else b for a, b in zip(A, B)]
    elif rop == 5: rgb = [a if a >= b else b for a, b in zip(A, B)]
    elif rop == 7: rgb = [A[i] if C[i] > 0.5 else B[i] for i in range(3)]
    elif rop == 8: rgb = [A[i] if C[i] >= 0 else B[i] for i in range(3)]
    elif rop == 9: rgb = [a - math.floor(a) for a in A]
    elif rop == 10:
        if sop is None: raise Unsupported('RGB_SOP without an alpha transcendental')
        rgb = [sop] * 3
    if aop == 0: al = aA * aB + aC
    elif aop == 1:
        if dp is None: raise Unsupported('ALPHA_DP without an RGB dot product')
        al = dp
    elif aop == 2: al = min(aA, aB)
    elif aop == 3: al = max(aA, aB)
    elif aop == 5: al = aA if aC > 0.5 else aB
    elif aop == 6: al = aA if aC >= 0 else aB
    elif aop == 7: al = aA - math.floor(aA)
    elif 8 <= aop <= 13: al = sop
    else: raise Unsupported('reserved alpha op %d' % aop)
    rom, aom = bits(rgb_inst, 28, 26), bits(alpha_inst, 28, 26)
    rgb = [_omod(v, rom) for v in rgb]; al = _omod(al, aom)
    if bits(cmn, 19, 19) and rom != 7: rgb = [min(1.0, max(0.0, v)) for v in rgb]
    if bits(cmn, 20, 20) and aom != 7: al = min(1.0, max(0.0, al))
    rgb = [f32(v) for v in rgb]; al = f32(al)
    if typ == 0:
        if bits(cmn, 17, 15) or bits(cmn, 18, 18): raise Unsupported('ALU OMASK (predicate update)')
        wm, rd, ad = bits(cmn, 13, 11), bits(rgba, 10, 4), bits(alpha_inst, 10, 4)
        if bits(rgba, 11, 11): raise Unsupported('relative destination')
        for i in range(3):
            if wm >> i & 1: px.t[rd][i] = rgb[i]
        if bits(cmn, 14, 14): px.t[ad][3] = al
    else:   # OUTPUT: RGB goes to rgb target, alpha to alpha target
        rt, at = bits(rgb_inst, 30, 29), bits(alpha_inst, 30, 29)
        om = bits(cmn, 17, 15)
        o = px.out.setdefault(rt, [None] * 4)
        for i in range(3):
            if om >> i & 1: o[i] = rgb[i]
        if bits(cmn, 18, 18): px.out.setdefault(at, [None] * 4)[3] = al


def _tex(px, ins):
    cmn, ti, ta = ins
    op = bits(ti, 24, 22)
    src = px.t[bits(ta, 6, 0)]
    if bits(ta, 7, 7): raise Unsupported('relative texture source')
    st = [src[bits(ta, 9 + 2 * k - 1, 8 + 2 * k - 2 + 0)] if False else src[bits(ta, 9 + 2 * k, 8 + 2 * k)] for k in range(4)]  # S,T,R,Q
    if op == 0: return
    if op == 2:
        if any(v < 0 for v in src): px.killed = True
        return
    if op == 3: st = [st[0] / st[3], st[1] / st[3], st[2] / st[3], st[3]]
    if px.sampler is None: raise Unsupported('texture lookup without a sampler callback')
    texel = px.sampler(bits(ti, 19, 16), st[0], st[1], st[2], op)
    d = px.t[bits(ta, 22, 16)]
    if bits(ta, 23, 23): raise Unsupported('relative texture destination')
    wm = bits(cmn, 13, 11); wa = bits(cmn, 14, 14)
    for i in range(3):
        if wm >> i & 1: d[i] = f32(texel[bits(ta, 25 + 2 * i - 1 + 0, 24 + 2 * i)] if False else texel[bits(ta, 25 + 2 * i, 24 + 2 * i)])
    if wa: d[3] = f32(texel[bits(ta, 31, 30)])


def run(program, px):
    for ins in program:
        step(px, ins)
        if bits(ins[0], 8, 8) and False: break
    return px


def disasm(ins):
    cmn = ins[0]; typ = ['ALU', 'OUT', 'FC', 'TEX'][bits(cmn, 1, 0)]
    if typ in ('ALU', 'OUT'):
        rop = ['MAD', 'DP3', 'DP4', 'D2A', 'MIN', 'MAX', '?', 'CND', 'CMP', 'FRC', 'SOP', 'MDH', 'MDV'][min(bits(ins[5], 3, 0), 12)]
        aop = ['MAD', 'DP', 'MIN', 'MAX', '?', 'CND', 'CMP', 'FRC', 'EX2', 'LN2', 'RCP', 'RSQ', 'SIN', 'COS', 'MDH', 'MDV'][bits(ins[4], 3, 0)]
        return '%s rgb:%s alpha:%s wmask=%x/%x omask=%x/%x last=%d' % (typ, rop, aop, bits(cmn, 13, 11), bits(cmn, 14, 14), bits(cmn, 17, 15), bits(cmn, 18, 18), bits(cmn, 8, 8))
    return typ
