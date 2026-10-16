#!/usr/bin/env python3
"""Extract and disassemble the fragment programs the stock driver uploads (GA_US_VECTOR_INDEX 0x4250 / DATA 0x4254) from a capture
(stage3i/3j/3k format). Programs are 6 dwords per instruction. Usage: decode_us.py CAPTURE.bin"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import r5xx_us as u

def load(path):
    d = open(path, 'rb').read(); link, cur, lim, magic, _ = struct.unpack('>5I', d[:20]); assert magic == 0x50414330
    b = d[20:]; return link, cur, lim, [struct.unpack('>I', b[i:i+4])[0] for i in range(0, len(b) - len(b) % 4, 4)]

def programs(words):
    out = []
    for i, w in enumerate(words):
        if w & 0xffff == 0x9095 and (w >> 30) == 0:                 # Type-0, base 0x1095, ONE_REG_WR
            n = ((w >> 16) & 0x3fff) + 1
            idx = None
            if i >= 2 and words[i - 2] & 0xffff == 0x1094: idx = words[i - 1]
            out.append((i, idx, words[i + 1:i + 1 + n]))
    return out

if __name__ == '__main__':
    link, cur, lim, words = load(sys.argv[1])
    for i, idx, d in programs(words):
        print('@word %d: US_VECTOR_INDEX=%s, %d dwords = %.2f instructions' % (i, hex(idx) if idx is not None else '?', len(d), len(d) / 6.0))
        for k in range(0, len(d) - len(d) % 6, 6):
            ins = d[k:k + 6]
            print('  [%2d] %s   %s' % (k // 6, ' '.join('%08x' % x for x in ins), u.disasm(tuple(ins[:3]) if (ins[0] & 3) == 3 else tuple(ins))))
