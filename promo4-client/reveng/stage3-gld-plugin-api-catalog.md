# Real catalog: ATIRadeonX1000GLDriver.bundle's full `_gld*` plugin API surface

Pure static RE (Ghidra symbol enumeration, zero hardware risk), done while chasing the open
"what buffer-management path does a plain `glClear` use" question from
`stage3g-cursor-field-misidentified.md`. Answers a structural question this whole project's docs never
directly stated: **this bundle is a renderer PLUGIN with a fixed callback API, not a per-GL-command
dispatcher**. All 62 real exported `_gld*` symbols, by category:

**Context/pixel-format lifecycle**: `_gldChoosePixelFormat`, `_gldDestroyPixelFormat`,
`_gldCreateContext`, `_gldDestroyContext`, `_gldReclaimContext`, `_gldCreateShared`,
`_gldDestroyShared`, `_gldInitializeLibrary`, `_gldTerminateLibrary`, `_gldGetRendererInfo`,
`_gldGetVersion`, `_gldGetString`, `_gldGetError`, `_gldGetInteger`, `_gldSetInteger`.

**Frame lifecycle**: `_gldFlush`, `_gldFinish`, `_gldFinishObject`, `_gldTestObject`.

**Framebuffer/drawable**: `_gldCreateFramebuffer`, `_gldDestroyFramebuffer`,
`_gldReclaimFramebuffer`, `_gldAttachDrawable`.

**Buffer objects** (real, not yet explored this project - a genuine VBO/PBO-style API):
`_gldCreateBuffer`, `_gldDestroyBuffer`, `_gldReclaimBuffer`, `_gldFlushBuffer`, `_gldPageoffBuffer`.

**Vertex arrays** (also real, unexplored - separate from the buffer-object API above):
`_gldCreateVertexArray`, `_gldDestroyVertexArray`, `_gldModifyVertexArray`,
`_gldReclaimVertexArray`, `_gldFlushVertexArray`, `_gldAllocVertexBuffer`,
`_gldCompleteVertexBuffer`, `_gldFreeVertexBuffer`.

**Textures**: `_gldCreateTexture`, `_gldDeleteTexture`, `_gldReclaimTexture`, `_gldModifyTexture`,
`_gldIsTextureResident`, `_gldCreateTextureLevel`, `_gldDeleteTextureLevel`,
`_gldModifyTextureLevel`, `_gldGetTextureLevel`, `_gldGetTextureLevelInfo`.

**Pipeline programs (GLSL)**: `_gldCreatePipelineProgram`, `_gldDestroyPipelineProgram`,
`_gldModifyPipelineProgram`, `_gldGetPipelineProgramInfo`, `_gldRelatePipelineProgram`.

**Fences and queries** (real synchronization/timing primitives - `_gldCreateFence`/
`_gldDestroyFence` are the fence mechanism `stage3-native-shader-attempt.md` speculated about without
a confirmed name; `_gldCreateQuery`/`_gldDestroyQuery`/`_gldGetQueryInfo` are a separate, entirely
unexplored occlusion/timing-query API): `_gldCreateFence`, `_gldDestroyFence`, `_gldCreateQuery`,
`_gldDestroyQuery`, `_gldGetQueryInfo`.

**Dispatch machinery**: `_gldInitDispatch`, `_gldUpdateDispatch` (the lazy-JIT dispatch-switch chain
this and earlier sessions have traced parts of).

**Memory-plugin hooks** (real, unexplored - looks like a separate pluggable allocator interface):
`_gldGetMemoryPluginData`, `_gldSetMemoryPluginData`, `_gldTestMemoryPluginData`,
`_gldFinishMemoryPluginData`, `_gldDestroyMemoryPluginData`.

## Why this explains the unresolved "+0x1c" mystery, and why it can't be resolved further statically

Notably absent: no `_gldClear`, `_gldDraw*`, `_gldBegin`/`_gldEnd`, or any per-GL-command entry point
at all. Combined with the already-known lazy dispatch-switch chain (`_gldUpdateDispatch` ->
`_gldInitDispatch` -> `glpPPShaderToProgram`, found via live gdb tracing in an earlier session - the
real "glClear_Exec" name that session saw came from a live backtrace, not this binary's static symbol
table), the real conclusion is structural: **individual GL commands like `glClear` don't have a
dedicated, statically-named implementation in this bundle at all** - they're serviced by anonymous
internal functions selected/assembled via the dispatch-switch machinery based on current GL state, and
reached only through function pointers in a runtime-built dispatch table.

This means the open question of exactly which code path a plain `glClear` takes (and why its buffer
read a header byte inconsistent with `FUN_0002c790`'s unconditional zeroing) **cannot be resolved by
further static symbol search** - there's no name to search for. Answering it needs either live dynamic
tracing (gdb, requires the G5) or manually following the dispatch-table's function pointers from a live
or dumped `AGLContext`/dispatch-table snapshot, which is a much larger undertaking (comparable to the
kext's already-documented 6,354-line `process_command_buffer`/`write_r500_3d_blit_state_packet` decompiles
in `stage3-scope-assessment.md`). Flagging this as the honest limit of this specific static-RE thread
rather than continuing to guess.
