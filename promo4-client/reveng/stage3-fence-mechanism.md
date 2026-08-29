# Real fence/synchronization mechanism found - a proper primitive for future injection designs

Pure static RE (Ghidra, zero hardware risk), done while the G5 remains down for an extended physical
power-cycle wait. Decompiled `_gldCreateFence`/`_gldDestroyFence`/`_gldFinishObject`/`_gldTestObject`
and their shared helper `FUN_0001a0d0` in `ATIRadeonX1000GLDriver.bundle` - real names known since
`stage3-gld-plugin-api-catalog.md`, not yet decompiled until now. This resolves a mechanism
`stage3-native-shader-attempt.md` only speculated about by name ("a real, native synchronization
mechanism... alongside the two already-guessed mechanisms") - it's real, and here's exactly how it
works.

## The real mechanism

**Fence creation** (`_gldCreateFence`, `00007850`): allocates an 8-byte CPU-side fence object and finds
a free slot in memory type 4 (the fence region, confirmed back in Stage 0 as "a CPU-side bitmap tracks
free slots directly against this region's own layout" - this decompile shows exactly that bitmap
allocator, `param_1+0x220`). Each fence slot in the mapped fence region is 8 bytes:
```c
*(undefined4 *)(slot*8 + fenceRegionBase)     = *(undefined4 *)(commandBufferBase + 0x18);  // saved tag
*(undefined4 *)(slot*8 + fenceRegionBase + 4) = 0;                                          // "needs flush" flag
```
`commandBufferBase + 0x18` is the current command buffer's own per-context tag field - the exact same
field `stage2-prep-command-buffer.md`'s kernel decompile already found (`init_command_buffer_header`:
`*(param_1+0x18) = *(this+0x7c)`, "per-context tag"). So creating a fence just snapshots "which buffer
generation is in flight right now."

**Checking/waiting** (`_gldTestObject`/`_gldFinishObject`, object type 0 = fence): both check whether
the fence's saved tag has already completed via `FUN_0001a0d0`:
```c
undefined4 FUN_0001a0d0(int param_1, int param_2 /* saved tag */) {
  byte *pbVar1 = *(byte **)(param_1 + 0x238);
  uint liveCompletedTag = (uint)pbVar1[3]<<24 | (uint)pbVar1[2]<<16 | (uint)pbVar1[1]<<8 | *pbVar1;
  return (int)(param_2 - liveCompletedTag) < 1;   // true if saved tag <= live completed tag
}
```
`param_1+0x238` is a **live, GPU-hardware-updated completion counter** - written directly by the GPU
(DMA/interrupt-style, the classic real fence-counter pattern) in the hardware's own native byte order.
The manual byte-by-byte reassembly (`pbVar1[3]<<24 | pbVar1[2]<<16 | ...`) is a real, necessary
endianness swap: this PowerPC code cannot just do a native 32-bit load of a value the GPU wrote in its
own (little-endian) byte order. The same class of bug/necessity as this user's own prior work on a
different project (Godot's CoreAudio big-endian format-flag fix) - a recurring real theme when PowerPC
software consumes hardware-written buffers.

If the fence hasn't completed yet, `_gldFinishObject` (the blocking "finish" variant) calls a real,
**previously undocumented external method - selector `9`**:
```c
_io_connect_method_scalarI_structureI(connectHandle, 9, slot*8 + fenceRegionBase, 1, 0, 0);
```
A genuine, real "block until this fence region offset signals" kernel call - a proper, targeted wait
primitive, distinct from `_gldFinish`'s selector `8` ("wait for literally everything," used everywhere
else in this project's own test programs so far).

Both `_gldTestObject`/`_gldFinishObject` first check a per-fence "needs flush" flag (fence slot's
second dword) and call `_gldFlush()` if set - i.e. if nothing has actually been submitted since the
fence was created, waiting on it would hang forever without first flushing, so the driver flushes for
you automatically.

## Real, useful new fields (AGLContext-relative, via the already-confirmed `+0x1600` delta)

| Driver-local | AGLContext-relative | Real meaning |
|---|---|---|
| `+0x218` | `+0x1818` | fence region base (memory type 4's mapped address) |
| `+0x238` | `+0x1838` | **live GPU completion-counter pointer** - read this directly (with the byte-swap above) for real-time hardware progress, no fence object needed |
| `commandBufferBase+0x18` | (base-relative, not ctx-relative) | current buffer's per-context tag - increments each real generation |

Not yet re-verified on hardware (G5 down) - this is a decompiled, internally-consistent mechanism, not
yet cross-checked live the way the cursor/chain-link fields were.

## Checked whether GL buffer objects (VBO/PBO-style) offer a simpler, independent target - they don't

Decompiled `_gldCreateBuffer`/`_gldFlushBuffer`/`_gldPageoffBuffer`/`_gldDestroyBuffer` hoping the
separate buffer-object API (`stage3-gld-plugin-api-catalog.md`) might expose a simpler memory region,
independent of the command-buffer chain-link machinery, worth targeting instead. **Real, honest
negative result**: it isn't independent. `_gldCreateBuffer` is a trivial 0x24-byte CPU-side bookkeeping
struct (type/usage fields, no hardware-adjacent behavior). `_gldPageoffBuffer` (the real teardown path)
uses the *exact same* primitives already documented above - the same `base+0x28 < cursor` check calling
`FUN_0001a0f0`, and the same fence-tag-check-then-selector-9-wait pattern via `FUN_0001a0d0`, seen
twice more in this one function. Also surfaces one more previously-undocumented external method,
**selector `0xd`** (13), called during buffer-object paging-off with a 2-word structure input
(`*puVar3`, `0`) - not yet decoded further, but confirms the fence/tag/selector-9 idiom is a real,
consistently-reused pattern across this driver's whole surface, not a one-off. **Conclusion**: there is
no simpler, hardware-adjacent memory region to sidestep the command-buffer/chain-link system with -
every path this project has found funnels through the same primitives, so a future attempt needs to
work correctly with those primitives, not around them.

## Why this matters for a future injection redesign

This doesn't fix the root cause already found and fixed conceptually (`stage3g-cursor-field-
misidentified.md` - writing through the wrong field, corrupting the chain-link pointer, is a
data-correctness bug, not a timing bug). But it's a real, proper tool worth using in the next design
regardless: rather than the ad hoc "settle to plain path, inject, immediately rebind and redraw"
sequence that led to both hangs, a future attempt could snapshot the buffer's tag before injecting,
flush, and then explicitly wait (via selector 9, or by polling `+0x1838` directly) for the GPU to
genuinely finish consuming that exact buffer generation before doing anything else with the context -
removing any ambiguity about whether prior content has actually been processed, independent of and in
addition to fixing the field-identity bug itself.
