#!/usr/bin/env python3
"""Layer 1b of the PROMO4 simulation (issue #1): replay a REAL captured live-AGLContext command buffer (stage3i-state-dump, taken
unflushed with the corrected field map) through the stock kernel walker in the emulator, with and without extra PM4 words appended
at the live cursor, using the driver's own flush convention (verified in the live probe: a pending record header sits at
ctx+0x17d8 with length 0; at flush its length is OR'd in as (end-link)/4; a length-0 header at the end terminates the walk).

Checks that the kernel's output for the injected buffer == the control output with exactly our words inserted, i.e. that appending at
the cursor (and advancing ctx+0x17dc only) cannot disturb the surrounding records.
Usage: inject_sim.py CAPTURE.bin PM4HEX      (PM4HEX as gate.py)
"""
import sys, os, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dryrun, pm4check

def load(path):
    d = open(path, 'rb').read()
    link, cur, lim, magic, _ = struct.unpack('>5I', d[:20])
    assert magic == 0x50414330, 'bad capture magic'
    buf = d[20:]
    return link, cur, lim, [struct.unpack('>I', buf[i:i+4])[0] for i in range(0, len(buf) - len(buf) % 4, 4)]

def build(words, link, cur, extra):
    w = list(words)
    li, ci = link // 4, cur // 4
    assert w[li] & 0xffffff == 0, 'pending header already has a length: %08x' % w[li]
    end = ci + len(extra)
    w = w[:ci] + list(extra) + [0x01000000]          # pending header gets its length; terminator = length-0 record
    w[li] |= (end - li)
    return w[7:]                                     # kernel walks from BUF+0x1c

def main():
    cap, hexs = sys.argv[1], sys.argv[2]
    link, cur, lim, words = load(cap)
    pm4 = [int(x, 0) for x in hexs.replace(' ', ',').split(',') if x]
    e, wn, _ = pm4check.check(pm4)
    print('capture: link=+0x%x cursor=+0x%x limit=+0x%x, %d words; pending header %08x; injecting %d dw' % (link, cur, lim, len(words), words[link // 4], len(pm4)))
    for x in e: print('PM4 ERROR:', x)
    ctl = dryrun.run(build(words, link, cur, []))
    inj = dryrun.run(build(words, link, cur, pm4))
    for nm, r in (('control ', ctl), ('injected', inj)):
        print('%s status=%s insns=%d submit=[%08x, %d dw] autopages=%s outside=%s' % (nm, r['status'], r['insns'], r['addr'], r['size_dw'], r['autopages'][:3], r['outside_calls'][:3]))
    bad = list(e)
    if ctl['status'] != 'RET' or inj['status'] != 'RET': bad.append('emulator did not return cleanly')
    # the fake self has no vtable, so the real stream's own opcode handlers (e.g. 0x2f's virtual call) already touch stub memory in the CONTROL run;
    # what matters is that the injection adds nothing new
    if set(inj['autopages']) != set(ctl['autopages']) or inj['outside_calls'] != ctl['outside_calls']: bad.append('injection changed which memory/calls the kernel touches: %s vs control %s' % (inj['autopages'], ctl['autopages']))
    if ctl['submitted'] is None or inj['submitted'] is None: bad.append('submit range invalid')
    else:
        c, i = ctl['submitted'], inj['submitted']
        # our words land where the control's terminator pad starts; everything before must be identical
        k = len(c)
        # control ends with its pad/terminator handling; find the common prefix
        p = 0
        while p < min(len(c), len(i)) and c[p] == i[p]: p += 1
        ins_at = (cur - 0x20) // 4
        print('common prefix %d dw (cursor is at submit-dword %d); control %d dw, injected %d dw' % (p, ins_at, len(c), len(i)))
        if p < ins_at: bad.append('buffer BEFORE the injection point differs between control and injected (first diff at dw %d)' % p)
        got = i[ins_at:ins_at + len(pm4)]
        if got != pm4: bad.append('injected words do not appear verbatim at the cursor: %s' % [hex(x) for x in got])
        else: print('injected words appear verbatim in the kernel output at submit-dword %d' % ins_at)
        tail = i[ins_at + len(pm4):]
        if tail not in ([], [0x80000000]): bad.append('unexpected words after the injected payload: %s' % [hex(x) for x in tail])
        if len(i) != len(c) + len(pm4) + len(tail) - (len(c) - ins_at): bad.append('submit length bookkeeping mismatch')
    print('INJECT-SIM OK' if not bad else 'INJECT-SIM FAIL: ' + '; '.join(bad)); return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
