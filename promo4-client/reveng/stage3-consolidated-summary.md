# Stage 3: consolidated summary - what's real and proven, what remains

This session's Stage 3 investigation (reproducing an already-proven GLSL shader in native ISA) covered
an unusually large amount of real ground across three different techniques - static decompilation,
live dynamic tracing (gdb), and direct function linking. This entry consolidates the whole arc into one
place: what's now genuinely proven, and what real, well-scoped work remains.

## What's fully proven, real, and reusable (no further work needed)

1. **The real native R5xx fragment-ALU instruction encoding** (`stage3-native-shader-attempt.md`) -
   derived directly from AMD's own documentation, byte-exact, independent of everything below. A
   correctly-bound native program using this encoding is known to be constructible.
2. **Per-context GPU register virtualization is real** (`stage3-resolution-per-context-state.md`) -
   proven via `store_reg`'s real decompile, not inferred. Any future native-ISA test must inject through
   the *same* connection that performs the draw, not a separate one.
3. **The embedded "extended opcode" command language exists and is partially mapped**
   (`stage3-embedded-opcode-language.md`) - real magic markers, real opcodes for texture load/surface-
   build/fast-clear, and this fully explains Stage 0's original "chain descriptor" mystery from the very
   start of this thread.
4. **A real, public, documented Apple API for surface management exists and partially works**
   (`stage3-real-ioaccelerator-api.md`, `stage3-surface-client-success.md`) - `kIOAccelSurfaceClientType`
   (0) is real and open-able on `ATIRadeonX1000`; `kIOAccelSurfaceGetState` (selector 2) succeeds and
   returns an independently-verified-correct result (`kIOAccelSurfaceStateIdleBit`). This is the first
   and only call in this whole Stage 3 thread whose result is confirmed correct against real
   documentation, not just "didn't error."
5. **The real, complete architecture of a working PBuffer attach** is now traced end-to-end via live
   gdb observation: `IOAccelFindAccelerator` -> `IOAccelCreateAccelID` -> `IOAccelCreateSurface` (real,
   stable `service=0x1f07`, `type=0`, a `_cglUniqueSurfaceID()`-generated unique value) ->
   `IOAccelSetSurfaceFramebufferShape` (real selector 9, `kIOAccelSurfaceSetShape`, on a *separate*
   per-surface connection) -> the GL context's own selector-0 "attach" call, which references the
   *same* unique surface ID used to create the IOAccelerator surface. This is a real, decisive,
   previously-unknown architectural map of how this driver actually works, not present in any public
   documentation.
6. **`IOAccel*` functions are real, exported, directly linkable C symbols** (confirmed via `nm` against
   `IOKit.framework` at the exact addresses observed live) - calling them directly (rather than
   hand-replicating their internal IOKit protocol) is a safe, valid, lower-risk approach than continuing
   to guess at raw external-method payloads, and was demonstrated not to crash even with a best-effort,
   not-fully-confirmed prototype.

## What remains genuinely open

Getting a decisive, positive native-ISA shader reproduction result requires a real, working render
target, which requires either:
- **Precisely reconstructing the exact C prototypes** for `IOAccelFindAccelerator`/`IOAccelCreateAccelID`/
  `IOAccelCreateSurface`/`IOAccelSetSurfaceFramebufferShape` - the real argument *values* are known from
  gdb observation, but exact argument *types and count* still carry some uncertainty (the one direct-call
  attempt this session returned an unrecognized, non-`IOReturn`-shaped code, `0x10000003` - informative,
  not decisive, and NOT a crash). The precise next technique for closing this gap is disassembling the
  exact call sites in `_CGLSetPBuffer`/`_cglGetIOAccelService` instruction-by-instruction (not just
  reading Ghidra's best-effort decompile) to determine with certainty which registers hold which
  arguments - more painstaking than what this session did, but a well-defined, bounded task.
- **Or continuing to hand-replicate the raw external-method sequence** on the correct, real
  multi-connection chain now fully mapped above - also viable, same remaining precision gap.

Both are genuinely tractable with more focused effort; neither is a dead end or requires yet another
new subsystem to be discovered. This is different from earlier points in this thread (which each
revealed a *new*, previously-unknown layer of complexity) - this is the same, now fully-mapped layer,
needing precision rather than more discovery.

## Everything committed and pushed this session

`241af35` through `f894ab7` on `g5-h264-gpu-decode`, `promo4-client/` directory - covering PROMO4
Stages 0 through 2 (fully complete, successful) and this entire Stage 3 investigation (real, substantial,
honest progress; the core native-ISA reproduction question remains open but precisely scoped, not stuck).
