#!/usr/bin/env python3
"""Layer 1c: like inject_sim.py but with the driver's LATER records present. Takes two read-only captures of the same run:
   PRE  (cursor/link at the injection point)   and   POST (the buffer after the draw was queued, before the flush).
Inserts PM4 at the PRE cursor position inside POST (the driver would have written its later records after our words), adds len(pm4) to the
length of the record that contains the cursor, runs the stock kernel walker on control and injected, and requires: identical output before
the insertion point, our words verbatim, and the control's remaining output (all later records, rewritten by the kernel) identical after.
Usage: inject_sim2.py PRE.bin POST.bin PM4HEX"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dryrun, pm4check, inject_sim

def stream(words, link, cur, extra, ins_cur, ins_link):
    w = list(words); li, ci = link // 4, cur // 4
    assert w[li] & 0xffffff == 0, 'final pending header has a length'
    w = w[:ci] + [0x01000000]
    w[li] |= (ci - li)                                # control flush convention (cursor == end)
    if extra:
        ii, ic = ins_link // 4, ins_cur // 4
        assert w[ii] & 0xffffff, 'record at the insertion link has no length yet'
        w = w[:ic] + list(extra) + w[ic:]
        w[ii] += len(extra)                           # that record's length grows by our words
    return w[7:]

def main():
    pre, post, hexs = sys.argv[1:4]
    pl, pc, _, pw = inject_sim.load(pre); ql, qc, _, qw = inject_sim.load(post)
    pm4 = [int(x, 0) for x in hexs.replace(' ', ',').split(',') if x]
    e, _, _ = pm4check.check(pm4); bad = list(e)
    if qw[:pc // 4] != pw[:pc // 4] and qw[:pl // 4] != pw[:pl // 4]: bad.append('POST capture does not extend PRE capture')
    ctl = dryrun.run(stream(qw, ql, qc, [], pc, pl)); inj = dryrun.run(stream(qw, ql, qc, pm4, pc, pl))
    print('PRE link=+0x%x cursor=+0x%x ; POST link=+0x%x cursor=+0x%x (%d words)' % (pl, pc, ql, qc, len(qw)))
    for nm, r in (('control ', ctl), ('injected', inj)): print('%s status=%s submit=%d dw autopages=%s' % (nm, r['status'], r['size_dw'], r['autopages'][:3]))
    if ctl['status'] != 'RET' or inj['status'] != 'RET': bad.append('emulator did not return cleanly')
    if set(ctl['autopages']) != set(inj['autopages']) or ctl['outside_calls'] != inj['outside_calls']: bad.append('injection changed touched memory/calls')
    if ctl['submitted'] is None or inj['submitted'] is None: bad.append('invalid submit range')
    else:
        c, i = ctl['submitted'], inj['submitted']; at = (pc - 0x20) // 4; n = len(pm4)
        if c[:at] != i[:at]: bad.append('output BEFORE the insertion point differs')
        if i[at:at + n] != pm4: bad.append('injected words not verbatim at the insertion point: %s' % [hex(x) for x in i[at:at + n]])
        if i[at + n:at + n + len(c) - at] != c[at:]: bad.append('LATER records changed by the insertion (kernel output after our words != control output after the same point)')
        extra = len(i) - len(c) - n
        if extra not in (0, 1, -1): bad.append('unexpected size delta %d' % (len(i) - len(c)))
        print('control %d dw, injected %d dw (delta %d for %d inserted)' % (len(c), len(i), len(i) - len(c), n))
    print('INJECT-SIM2 OK' if not bad else 'INJECT-SIM2 FAIL: ' + '; '.join(bad)); return 1 if bad else 0

if __name__ == '__main__': sys.exit(main())
