# Stage 3: real, live dynamic trace of the attach-drawable sequence (gdb)

Follow-up to `stage3-attach-drawable-selectors.md`'s empirical probe (which failed with
`kIOReturnBadArgument` on guessed parameters). Rather than continue guessing, used gdb 6.3 (already
proven on this exact G5 earlier in this project's history) to break on the real, live external-method
entry points inside a real, already-working AGL PBuffer context (`hold-context`) and capture the true
argument values as Apple's own code actually uses them. Pure observation - no modification, no new
hardware risk beyond what `hold-context` already does routinely.

## Method

Broke at `main`, let shared libraries load, then set breakpoints directly on the raw entry addresses of
`io_connect_method_scalarI_structureI` and `io_connect_method_scalarI_scalarO` (note: gdb strips the
leading underscore Ghidra shows) - printing `r3`-`r10` (the first 8 integer argument registers per the
classic PowerPC/Mac OS X ABI) plus a memory dump at the struct-pointer register for `structureI` calls.
Confirmed breaking at the literal symbol address (before gdb's automatic prologue-skip) gives identical
register values to the post-prologue breakpoint - the prologue doesn't disturb the original arguments
here, so both are reliable.

## Real, complete captured sequence for the main PBuffer context (single `hold-context` run)

```
conn=0x220b  sel=3   (scalarO)                              <- separate, earlier connection (context creation's own query)
conn=0x220f  sel=3   (scalarO)                               <- main connection, same query already confirmed (Stage 0/1)
conn=0x220f  sel=20  (scalarO)                                <- real, newly observed selector
conn=0x2403  sel=0   (struct, 2 bytes)                        <- a third, transient connection, DIFFERENT struct size (2, not 4)
conn=0x230b  sel=7   (scalarO)                                <- secondary connection (matches earlier static finding)
conn=0x230b  sel=9   (struct, 2 bytes) = 0x00000081            <- real, non-zero payload
conn=0x220f  sel=6   (scalarO, 1 in/1 out)
conn=0x220f  sel=0   (struct, 4 bytes) = 0x0cf4b554             <- THE REAL ATTACH CALL
conn=0x220f  sel=16  (struct, 1 byte)  = 0x00
conn=0x220f  sel=5   (scalarO)
conn=0x220f  sel=3   (scalarO)                                 <- same query again, post-attach
conn=0x220f  sel=4   (scalarO)
conn=0x220f  sel=8   (struct, 0 bytes) x3                      <- finish, once per real GL operation - EXACTLY matches
                                                                    this project's own selector-8 usage throughout
... at aglDestroyContext teardown ...
conn=0x220f  sel=0   (struct, 4 bytes) = 0x00000000             <- THE REAL DETACH/RESET CALL
```

## Real, decisive findings

- **Selector 0's real structure is 4 bytes (one dword), not 16** - Stage 3c's original probe sent a
  4x-oversized structure, which alone likely explains its `kIOReturnBadArgument` result.
- **Selector 0 has two real, distinct uses on the exact same shape**: a **detach/reset** call with
  payload `0` (clean, deterministic, reproduced identically at every real teardown - matches
  `_gldAttachDrawable`'s own decompiled cleanup path, `local_58=0`, exactly), and an **attach** call
  whose payload is a **live, per-run-varying value** (`0x0cf4b554` this run, `0x0cefe733` and
  `0x0ca95fd1` in two earlier runs - not a fixed constant). The consistent high byte (`0x0c`) across
  runs suggests a real address-range or handle-namespace pattern, not random noise - but the exact
  value cannot be fabricated without a real, live resource behind it.
- **Selector 16's real structure is 1 byte**, observed value `0`, consistent with the earlier static
  finding (`_gldAttachDrawable`: `local_58 = *(byte*)(param_1+0x147)`, a per-context flag defaulting to
  0 on a fresh context).
- **Selector 8 (finish) confirmed to match this project's own usage exactly** - called with a 0-byte
  structure every time, identical to every `selector 8` call this whole PROMO4 thread has made since
  Stage 0.
- **A third, previously-unseen selector, 20 (`0x14`), confirmed real** - scalarO shape, not yet
  characterized further.
- **Multiple real, distinct IOKit connections are involved in one PBuffer setup**, not just the one
  main GL context connection - confirms and extends the earlier "secondary connection" finding from
  static analysis (`stage3-attach-drawable-selectors.md`) with live, concrete evidence (three distinct
  `io_connect_t` values observed: `0x220b`, `0x220f`/main, `0x2403`, `0x230b`).

## Real, safe empirical re-test

Rebuilt `stage3c-attach-probe` with the corrected 4-byte/1-byte shapes and the real, reproducible
"detach" value (`0`) for selector 0, and the real observed value (`0`) for selector 16. **Still returned
`kIOReturnBadArgument` for both** on a fresh, minimal connection with no prior attach ever performed -
a real, informative result: the size correction alone wasn't sufficient. Consistent with the kernel
validating real precondition state (e.g. rejecting a "detach" when nothing was ever attached) that a
minimal connection never establishes. System stayed fully healthy throughout (post-probe selector-3
health check matched baseline exactly, as in every other test this thread).

## Honest status

The dynamic-trace technique worked cleanly and gave real, decisive, previously-unobtainable data - a
genuine methodological win for this investigation. But it confirms rather than overturns the earlier
scope assessment: a real, working attach requires the real, live IOAccelerator-created surface ID that
selector 0's non-zero payload encodes, which in turn requires replicating the real `IOAccelCreateSurface`
/`IOAccelSetSurfaceFramebufferShape` sequence found in `_CGLSetPBuffer`'s decompile - genuinely new,
substantial discovery work in yet another real Apple subsystem, not yet attempted.
