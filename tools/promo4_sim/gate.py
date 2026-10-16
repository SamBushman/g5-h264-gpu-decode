#!/usr/bin/env python3
"""The pre-flight gate (issue #1): a PM4 stream may be sent to the G5 only if BOTH layers pass.
  layer 2  pm4check.check   - packet structure + register policy
  layer 1  dryrun.run/check - stock kernel dispatcher in the PPC emulator
Usage: gate.py HEXWORDS | -f FILE [--allow-ps3]     exit 0 = cleared to send
"""
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import pm4check, dryrun

def gate(words, allow_ps3=False):
    e, w, sh = pm4check.check(words, allow_ps3)
    r = dryrun.run(dryrun.wrap(words)); k = dryrun.check(words, r)
    return e, w, k

if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if x != '--allow-ps3']
    txt = open(a[1]).read() if a[0] == '-f' else a[0]
    words = [int(x, 0) for x in txt.replace('\n', ',').replace(' ', ',').split(',') if x]
    e, w, k = gate(words, '--allow-ps3' in sys.argv)
    for x in w: print('WARN  [pm4]   ', x)
    for x in e: print('ERROR [pm4]   ', x)
    for x in k: print('ERROR [kernel]', x)
    ok = not e and not k
    print('GATE PASS: %d dwords cleared' % len(words) if ok else 'GATE FAIL'); sys.exit(0 if ok else 1)
