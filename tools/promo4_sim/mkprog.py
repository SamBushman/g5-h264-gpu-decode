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
if __name__ == '__main__':
    print(','.join('0x%08x' % w for w in prog(sys.argv[1])))
