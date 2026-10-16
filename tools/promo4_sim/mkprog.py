#!/usr/bin/env python3
"""Emit the PM4 for loading ONE fragment program into US slot 100 and pointing US_CODE_ADDR at it (same words as
src/stage3_native_shader.c emit_load_program, known-good on hardware as GLSL-equivalent). Usage: mkprog.py red|blue"""
import sys
def t0(base_idx, count_minus1, one=False): return (base_idx & 0x1fff) | ((1 if one else 0) << 15) | ((count_minus1 & 0x3fff) << 16)
CMN, RGBADDR, ALPHAADDR, ALPHAINST, RGBAINST = 0x001F8105, 0, 0, 0x00C18000, 0x20490000
RGB = {'red': 0x00DB0498, 'blue': 0x00DB0690}
def prog(color, slot=100):
    return [t0(0x1094, 0), slot, t0(0x1095, 5, True), CMN, RGBADDR, ALPHAADDR, RGB[color], ALPHAINST, RGBAINST,
            t0(0x118c, 0), (slot << 16) | slot]
# Texture programs. TEX words are EXACTLY what the stock compiler emitted for texture2DRect(t, gl_TexCoord[0].xy) (captured, stage13);
# the OUT words are the compiler's passthrough ('texpass') or the same with the RGB swizzles reversed ('texswap': out.rgb = texel.bgr).
TEX = [0x00007807, 0x0e400000, 0xe4000400, 0, 0, 0]
OUT_PASS = [0x00078105, 0x10040000, 0x10040000, 0x00db0220, 0x00c0c000, 0x20490000]
OUT_SWAP = [0x00078105, 0x10040000, 0x10040000, 0x00db0028, 0x00c0c000, 0x20490000]
def prog_tex(out, slot=100):
    code = TEX + out; n = len(code) // 6
    return [t0(0x1094, 0), slot, t0(0x1095, len(code) - 1, True)] + code + \
           [t0(0x118d, 0), ((n - 1) << 16) | slot, t0(0x118c, 0), ((slot + n - 1) << 16) | slot]
if __name__ == '__main__' and sys.argv[1] in ('texpass', 'texswap'):
    print(','.join('0x%08x' % w for w in prog_tex(OUT_PASS if sys.argv[1] == 'texpass' else OUT_SWAP))); sys.exit(0)
if __name__ == '__main__':
    print(','.join('0x%08x' % w for w in prog(sys.argv[1])))
