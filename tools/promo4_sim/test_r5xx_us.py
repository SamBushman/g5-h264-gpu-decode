"""Ground truth: the two programs stage3_native_shader.c ran on real hardware (red and blue control/test pixels)."""
import sys, os; sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r5xx_us as u
CMN, RGBA, ALPHA = 0x001F8105, 0x20490000, 0x00C18000
def prog(rgb_inst): return [(CMN, 0, 0, rgb_inst, ALPHA, RGBA)]
red = u.run(prog(0x00DB0498), u.Pixel()).out; blue = u.run(prog(0x00DB0690), u.Pixel()).out
print('RED  out:', red); print('BLUE out:', blue)
assert red[0][:3] == [1.0, 0.0, 0.0], red
assert blue[0][:3] == [0.0, 0.0, 1.0], blue
print('disasm:', u.disasm(prog(0x00DB0498)[0]))
# generic ALU sanity: r1 = r0*2 + 0.5 via MAD, then DP3 against a constant
px = u.Pixel(consts={0: [1, 2, 3, 0]}); px.t[0] = [0.25, 0.5, 1.0, 1.0]
# MAD r1 = src0*src1 + src2: src0=r0(temp0), src1=inline 1.0 (0xB8: exp 7, bias 7), src2=zero  ; sel A=src0 swiz rgb, B=src1 swiz rrr, C=src2 swiz zero
rgb_addr = 0 | (0xB8 << 10) | (0 << 20)
rgb_inst = (0) | (0<<2) | (1<<5) | (2<<8) | (1<<13) | (0<<15) | (0<<18) | (0<<21) | (7<<26)   # A=src0 (R,G,B), B=src1 (R,R,R), OMOD disabled
rgba = 0 | (1 << 4) | (2 << 12) | (4 << 14) | (4 << 17) | (4 << 20)                          # MAD -> r1 ; C=src2 zero
cmn = 0 | (7 << 11)                                                                           # ALU, write RGB
u.step(px, (cmn, rgb_addr, 0, rgb_inst, 0, rgba))
assert px.t[1][:3] == [0.25, 0.5, 1.0], px.t[1]
print('MAD ok:', px.t[1])
print('ALL OK')
# inline constant table from the doc: 0x0 -> 2^-10, (e0,m1) -> 2^-9, (e0,m2) -> 2^-8, (e1,m4)?? see note: denormal rule gives 2^-7
assert u.inline_const(0x00) == 2.0**-10 and u.inline_const(0x01) == 2.0**-9 and u.inline_const(0x02) == 2.0**-8
assert u.inline_const(0x38) == 1.0 and u.inline_const(0x78) == 256.0
assert u.inline_const(0x7f) == 480.0, u.inline_const(0x7f)   # doc table: exp 0xf mant 0x7 -> 480
assert u.inline_const(0x04) == 2.0**-7                        # doc table: denormal mant 4 -> 2^-7
print('inline constants ok')
