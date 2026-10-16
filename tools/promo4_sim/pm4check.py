#!/usr/bin/env python3
"""Layer 2 of the PROMO4 simulation (issue #1): static PM4 decoder / register-policy validator.

Decodes a PM4 word stream (Type-0/1/2/3, field layout from AMD's R5xx Acceleration doc 6.1 and confirmed against the kext's own
track_regs_written_by_pm4) and checks every register write against a policy built from Apple's own register sets
(reveng/stage4-radeon3DCopySetup-complete-draw-reference.md, stage4-complete-register-tracking-state-map.md) and the documented
hang risks (reveng/stage3-viewport-and-hang-mitigation-from-pdf.md).

ERROR  = never send (malformed packet, zero header, forbidden/unknown register block, unlisted Type-3 opcode, overrun)
WARN   = inside a legitimate 3D block but not a register this project has confirmed; needs a human decision
Usage: pm4check.py HEXWORDS | -f FILE   [--allow-ps3]
Exit 0 = no ERRORs.  As a library: check(words) -> (errors, warnings, shadow_regs)
"""
import sys

# byte address -> name (first of an array is expanded below). Sources: the two reveng tables above.
NAMED = {
 0x43a8:'SC_EDGERULE',0x43e8:'SC_SCREENDOOR',0x4010:'GB_MSPOS0',0x4014:'GB_MSPOS1',0x4020:'GB_AA_CONFIG',0x4008:'GB_ENABLE',
 0x43a4:'SC_HYPERZ_EN',0x43d0:'SC_CLIP_RULE',0x43e0:'SC_SCISSOR0',0x43e4:'SC_SCISSOR1',
 0x4f1c:'ZB_BW_CNTL',0x4f04:'ZB_ZSTENCILCNTL',0x4f00:'ZB_CNTL',0x4f10:'ZB_FORMAT',0x4f20:'ZB_DEPTHOFFSET',0x4f24:'ZB_DEPTHPITCH',
 0x4f28:'ZB_DEPTHCLEARVALUE',0x4f54:'ZB_HIZ_PITCH',
 0x2140:'VAP_CNTL_STATUS',0x221c:'VAP_CLIP_CNTL',0x20b0:'VAP_VTE_CNTL',0x2090:'VAP_OUT_VTX_FMT_0',0x2094:'VAP_OUT_VTX_FMT_1',0x20b4:'VAP_VTX_SIZE',
 0x42b8:'SU_CULL_MODE',0x4e50:'RB3D_DITHER_CTL',0x4e00:'RB3D_CCTL',0x4e04:'RB3D_BLENDCNTL',0x4e0c:'RB3D_COLOR_CHANNEL_MASK',0x4e18:'RB3D_ROPCNTL',
 0x4e28:'RB3D_COLOROFFSET0',0x4e38:'RB3D_COLORPITCH0',
 0x4bc0:'FG_FOG_BLEND',0x4bd4:'FG_ALPHA_FUNC',0x4288:'GA_POLY_MODE',0x4278:'GA_COLOR_CONTROL',0x4258:'GA_COLOR_CONTROL_PS3',
 0x4104:'TX_ENABLE',0x4100:'TX_INVALTAGS',0x4250:'GA_US_VECTOR_INDEX',0x4254:'GA_US_VECTOR_DATA',
 0x4600:'US_CONFIG',0x4604:'US_PIXSIZE',0x4624:'US_FC_CTRL',0x4630:'US_CODE_ADDR',0x4634:'US_CODE_RANGE',0x4638:'US_CODE_OFFSET',
 0x46a4:'US_OUT_FMT_0',0x46a8:'US_OUT_FMT_1',0x46ac:'US_OUT_FMT_2',0x46b0:'US_OUT_FMT_3',
 0x4300:'RS_COUNT',0x4304:'RS_INST_COUNT',0x4200:'GA_POINT_S0',0x4204:'GA_POINT_T0',0x4208:'GA_POINT_S1',0x420c:'GA_POINT_T1',0x421c:'GA_POINT_SIZE',
 0x2150:'VAP_PROG_STREAM_CNTL_0',0x21e0:'VAP_PROG_STREAM_CNTL_EXT_0',
}
ARRAYS = [(0x4320,16,'RS_INST'),(0x4074,16,'RS_IP'),(0x4400,16,'TX_FILTER0'),(0x4440,16,'TX_FILTER1'),(0x4480,16,'TX_FORMAT0'),
          (0x44c0,16,'TX_FORMAT1'),(0x4540,16,'TX_OFFSET'),(0x2150,8,'VAP_PROG_STREAM_CNTL'),(0x21e0,8,'VAP_PROG_STREAM_CNTL_EXT')]
for base, n, nm in ARRAYS:
    for i in range(n): NAMED.setdefault(base + 4*i, '%s_%d' % (nm, i))
# documented hang risk (AMD R5xx doc 10.9.5): refuse unless --allow-ps3
HANG_RISK = {0x411c:'PS3_VTX_FMT', 0x4120:'PS3_TEX_SOURCE', 0x4118:'GB_PS3_ENABLE (master enable next to the hang-risk pair)'}
# never from a client: soft reset and anything outside the 3D blocks
FORBIDDEN = {0x429c:'GA_SOFT_RESET'}
# legitimate 3D register spans (byte addr, inclusive-exclusive); a write inside one but not NAMED is a WARN
SPANS = [(0x2000,0x2300,'VAP'),(0x4000,0x4800,'GB/GA/SU/SC/RS/TX/US'),(0x4b00,0x4c00,'FG'),(0x4e00,0x4f00,'RB3D'),(0x4f00,0x5000,'ZB')]
# Type-3 opcodes a 3D client may emit (R3xx/R5xx numbering). NOP is the kernel's own skip marker.
T3_OK = {0x10:'NOP',0x26:'WAIT_FOR_IDLE',0x28:'3D_DRAW_VBUF',0x29:'3D_DRAW_IMMD',0x2a:'3D_DRAW_INDX',0x2f:'LOAD_VBPNTR',
         0x33:'INDX_BUFFER',0x34:'3D_DRAW_VBUF_2',0x35:'3D_DRAW_IMMD_2',0x36:'3D_DRAW_INDX_2'}


def span(addr):
    for lo, hi, nm in SPANS:
        if lo <= addr < hi: return nm
    return None


def reg_check(addr, errors, warnings, where, allow_ps3):
    if addr in FORBIDDEN: errors.append('%s: write to %s (0x%04x) is forbidden' % (where, FORBIDDEN[addr], addr)); return
    if addr in HANG_RISK and not allow_ps3: errors.append('%s: %s (0x%04x) is a documented hang risk' % (where, HANG_RISK[addr], addr)); return
    if span(addr) is None: errors.append('%s: register 0x%04x is outside every allowed 3D block' % (where, addr)); return
    if addr not in NAMED and addr not in HANG_RISK: warnings.append('%s: register 0x%04x (%s block) is not a confirmed register' % (where, addr, span(addr)))


def check(words, allow_ps3=False):
    errors, warnings, shadow = [], [], {}
    if not words: return ['empty stream'], warnings, shadow
    i, n = 0, len(words)
    while i < n:
        h = words[i]; t = h >> 30; where = 'dw%d(0x%08x)' % (i, h)
        if h == 0: errors.append('%s: all-zero header (also the kernel record terminator)' % where); i += 1; continue
        if t == 2:
            if h != 0x80000000: warnings.append('%s: Type-2 with non-zero low bits' % where)
            i += 1
        elif t == 0:
            cnt = ((h >> 16) & 0x3fff) + 1; base = (h & 0x1fff) * 4; one = bool(h & 0x8000)
            if i + 1 + cnt > n: errors.append('%s: Type-0 wants %d data dwords, only %d left' % (where, cnt, n - i - 1)); break
            for k in range(cnt):
                a = base if one else base + 4 * k
                reg_check(a, errors, warnings, where, allow_ps3); shadow[a] = words[i + 1 + k]
            i += 1 + cnt
        elif t == 1:
            if i + 3 > n: errors.append('%s: truncated Type-1' % where); break
            for r, v in (((h & 0x7ff) * 4, words[i+1]), (((h >> 11) & 0x7ff) * 4, words[i+2])):
                reg_check(r, errors, warnings, where, allow_ps3); shadow[r] = v
            i += 3
        else:
            op = (h >> 8) & 0xff; cnt = ((h >> 16) & 0x3fff) + 1
            if op not in T3_OK: errors.append('%s: Type-3 opcode 0x%02x not on the allowlist' % (where, op))
            if i + 1 + cnt > n: errors.append('%s: Type-3 body %d dwords overruns the stream (%d left)' % (where, cnt, n - i - 1)); break
            i += 1 + cnt
    return errors, warnings, shadow


def main():
    a = [x for x in sys.argv[1:] if x != '--allow-ps3']
    ps3 = '--allow-ps3' in sys.argv
    if not a: print(__doc__); return 2
    txt = open(a[1]).read() if a[0] == '-f' else a[0]
    words = [int(x, 0) for x in txt.replace('\n', ',').replace(' ', ',').split(',') if x]
    e, w, sh = check(words, ps3)
    print('%d dwords, %d distinct registers written' % (len(words), len(sh)))
    for x in w: print('WARN :', x)
    for x in e: print('ERROR:', x)
    print('PM4 OK' if not e else 'PM4 REJECTED'); return 1 if e else 0


if __name__ == '__main__':
    sys.exit(main())
