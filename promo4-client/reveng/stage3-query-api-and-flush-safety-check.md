# Real occlusion-query API, and a hard safety-exit found inside the flush path

Pure static RE (Ghidra, zero hardware risk), continuing the driver-bundle survey while the G5 remains
down. Two smaller but real findings.

## `FUN_0002c6c0`: the flush path enforces one invariant hard enough to kill the process

Called at the very end of `FUN_0001a0f0` (the real submit/flush function - see
`stage3g-cursor-field-misidentified.md`). Real decompile:
```c
void FUN_0002c6c0(int param_1) {
  puVar1 = *(undefined4 **)(param_1 + 0x208);   // the context/state buffer (memory type 2)
  puVar1[7] = puVar1[7] | 1;
  // re-point a pair of get-write-pointer/commit-write-pointer function pointers
  // (the same param_1+0x2998/0x299c/0x298c fields FUN_0001bac0 calls into) at a
  // fresh accessor pair, first a "growing" variant then, after FUN_00050300 runs,
  // a "fixed capacity" variant
  ...
  puVar1[5] = *(int *)(param_1 + 0x298c) - (int)(puVar1 + 8) >> 2;   // computed real capacity
  if ((uint)puVar1[4] < (uint)puVar1[5]) {
    _exit(1);        // hard, unconditional process termination - not a soft error return
  }
}
```
This is a genuine, deliberate invariant check baked into Apple's own driver: if the computed capacity
of this internal ring/accessor structure (embedded inside the memory-type-2 context buffer, alongside
the growth-sub-allocator `stage3g-cursor-field-misidentified.md`'s correction already found there)
exceeds an expected bound, the driver kills the whole process immediately rather than continue in a bad
state. **Relevant context for this whole investigation's root-cause finding**: this shows the driver
*does* have hard safety nets for some internal invariants - just not for the specific chain-link field
(`+0x1d8`) this project's `stage3g_real_injection.c` corrupted, which has no such check and instead
propagates silently into a kernel-side chain walk. The fragility that caused two real hangs isn't
because this driver never validates its own state; it's because that specific field isn't one of the
ones it validates.

## Real occlusion-query API (`GL_ARB_occlusion_query`), fully decoded

`_gldCreateQuery`/`_gldDestroyQuery`/`_gldGetQueryInfo` (`00009e70`/`00009c80`/`0002b5a0`) implement
real occlusion queries, confirmed by the real GL enum values used: `0x8866` =
`GL_QUERY_RESULT_ARB`, `0x8867` = `GL_QUERY_RESULT_AVAILABLE_ARB`. Query slots come from their own
independent bitmap allocator (`param_1+0x22c`/`+0x230`/`+0x224`/`+0x228` - the same bitmap-allocator
shape as the fence mechanism's `+0x21c`/`+0x220`, but a fully separate pool). Each query's result record
holds **four** sub-values (`piVar8[0]`/`[2]`/`[3]`/`[4]`) - plausibly per-GPU-pipe sample counts that get
combined, consistent with this being a multi-pipe R5xx part. `GL_QUERY_RESULT_AVAILABLE_ARB` polls
non-blockingly and flushes if nothing has been submitted yet (same "flush before you could possibly be
done" pattern as the fence mechanism); `GL_QUERY_RESULT_ARB` blocks via a real `_usleep(100)` polling
loop (up to `0x2710`/10000 iterations - i.e. up to ~1 second) rather than the fence mechanism's kernel
wait (`selector 9`) - a real, different (software-polling, not kernel-blocking) synchronization idiom
for this specific subsystem. Not directly relevant to the injection-safety question, but rounds out
this project's picture of the driver's real synchronization primitives (three distinct idioms now
found: `_gldFinish`'s selector-8 global wait, the fence mechanism's selector-9 targeted wait, and this
query mechanism's userspace polling loop).
