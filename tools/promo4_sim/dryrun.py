#!/usr/bin/env python3
"""Layer 1 of the PROMO4 simulation (issue #1): run a PM4 payload through the STOCK kext's real
ATIR500GLContext::process_command_buffer in the unicorn PPC emulator (kext-source's Tools/userspace/emu/kemu.py)
and report what the kernel would hand to the ring. Zero hardware risk.

Framing (verified in the emulator, see reveng/stage11-simulation-layer1.md): the kernel walks the buffer from BUF+0x1c as
length-prefixed records, header = opcode<<24 | length_in_dwords_including_header. A single opcode-0 record covering the whole
PM4 payload, followed by a 0 terminator, is passed through untouched; the submit range is [BUF+0x20, +4*size) and an odd
length is padded with one Type-2 filler (0x80000000).

Usage: dryrun.py HEXWORDS            e.g. dryrun.py 0x00010400,0x11111111,0x22222222
       dryrun.py --raw-record HDR,.. feed raw record words (hostile/negative tests)
Env:   KEMU_DIR (default ~/Documents/ATI-X1900-Decomp/atiradeonx1000-kext-source/Tools/userspace/emu), UPKGS (unicorn dir)
Exit status 0 = stream accepted exactly as intended; 1 = any anomaly (the reasons are printed).
"""
import os, sys, struct
KEMU_DIR = os.environ.get('KEMU_DIR', os.path.expanduser('~/Documents/ATI-X1900-Decomp/atiradeonx1000-kext-source/Tools/userspace/emu'))
sys.path[:0] = [os.environ.get('UPKGS', '/tmp/upkgs'), KEMU_DIR, os.path.join(KEMU_DIR, '..')]
import kemu
from unicorn.ppc_const import UC_PPC_REG_3  # noqa: F401  (import check)

TYPE2 = 0x80000000
REAL_BUF_BYTES = 0x20000   # the real mapped type-1 command buffer is 128KB (Stage 2a); payload capacity = (size-0x20)/4 dwords
CAP_DW = (REAL_BUF_BYTES - 0x20) // 4


def wrap(pm4):
    """Single opcode-0 pass-through record around the PM4 words."""
    return [len(pm4) + 1] + list(pm4)


def run(record_words, ctx='gl'):
    symname, off = kemu.DISPATCH[ctx]
    r = kemu.Runner(); uc = r.uc
    uc.mem_map(kemu.STOP & ~0xfff, 0x1000)
    entry = kemu.addr_of(r.m, symname)
    sb = bytearray(kemu.SELF_SIZE)
    struct.pack_into('>I', sb, off['bufoff'], kemu.BUF)
    struct.pack_into('>I', sb, off['accoff'], kemu.ACCEL)
    struct.pack_into('>I', sb, off['accoff2'], kemu.ACCEL)
    struct.pack_into('>I', sb, 0x108, kemu.ACCEL + 0x100)
    uc.mem_write(kemu.SELF, bytes(sb)); uc.mem_write(kemu.ACCEL, bytes(kemu.ACCEL_SIZE))
    stream = kemu.build_stream(list(record_words))
    bb = bytearray(kemu.BUF_SIZE); bb[0x1c:0x1c + len(stream)] = stream
    uc.mem_write(kemu.BUF, bytes(bb))
    desc = kemu.STACK + kemu.STACK_SIZE - 0x1000
    status = r.call(entry, kemu.SELF, desc)
    d = [r.word(desc + 4 * i) for i in range(4)]
    addr, size_dw = d[0], d[2]
    submitted = [r.word(addr + 4 * i) for i in range(size_dw)] if kemu.BUF <= addr < kemu.BUF + kemu.BUF_SIZE and size_dw < 0x10000 else None
    return dict(status=status, insns=r.n, addr=addr, size_dw=size_dw, cur=d[3], submitted=submitted,
                autopages=[hex(a) for a in r.autopages], outside_calls=[hex(a) for a in r.outside_calls])


def check(pm4, res):
    bad = []
    if res['status'] != 'RET': bad.append('emulator status %s' % res['status'])
    if res['autopages']: bad.append('kernel touched unmapped memory (would fault / hang on hardware): %s' % res['autopages'][:6])
    if res['outside_calls']: bad.append('unexpected calls outside the kext image: %s' % res['outside_calls'][:6])
    if len(pm4) + 1 > CAP_DW or res['size_dw'] > CAP_DW: bad.append('record/submit length %d dw exceeds the REAL 128KB buffer capacity (%d dw): kernel would read past the buffer' % (max(len(pm4)+1, res['size_dw']), CAP_DW))
    exp = list(pm4) + ([TYPE2] if len(pm4) % 2 else [])
    if res['submitted'] is None: bad.append('submit range (addr=%08x size=%d dw) outside the command buffer' % (res['addr'], res['size_dw']))
    elif res['submitted'] != exp: bad.append('submitted words differ from the intended PM4 (+pad)')
    if res['addr'] != kemu.BUF + 0x20: bad.append('submit range does not start at BUF+0x20')
    return bad


def main():
    a = sys.argv[1:]
    raw = bool(a and a[0] == '--raw-record')
    if raw: a = a[1:]
    words = [int(x, 0) for x in a[0].split(',')]
    if raw:
        res = run(words); res['submitted'] = None if res['submitted'] is None else '%d words' % len(res['submitted']); print(res); return 0
    res = run(wrap(words)); bad = check(words, res)
    print('status=%s insns=%d submit=[%08x, %d dw] pad=%s' % (res['status'], res['insns'], res['addr'], res['size_dw'], 'yes' if len(words) % 2 else 'no'))
    for b in bad: print('ANOMALY:', b)
    print('OK: kernel path accepts the stream as intended' if not bad else 'FAIL')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
