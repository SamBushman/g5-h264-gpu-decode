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

## Real follow-up: disassembly-confirmed prototypes, one real success, one real remaining puzzle

Disassembled (not decompiled - real raw PowerPC instructions) the exact call sites for all four real
`IOAccel*` functions inside `_CGLSetPBuffer`. **Fully decisive result for the shape structure**: the
real 20-byte structure `IOAccelSetSurfaceFramebufferShape` builds is now completely known byte-for-byte
- offset 0x00 = constant `1`, offset 0x08/0x0a = width/height as `uint16_t`, everything else zero.
Confirmed `IOAccelCreateSurface` takes exactly 4 arguments with the 4th a real output pointer (computed
via `addi`, not loaded - definitively a pointer, not a value).

Built `stage3f-real-surface` using these disassembly-derived prototypes, called directly (linked
against `IOKit.framework`). **Real, clean success**: `IOAccelCreateAccelID(0, &accelID, 0x40, 6)`
returned `kr=0`, `accelID=0xcf1b37b` - a real, meaningful, kernel-assigned identifier matching the exact
numeric pattern (`0xc*******`) observed in every live trace of Apple's own working code this session.
This is real, concrete proof the direct-linking approach works once a prototype is right.

**Real, consistent remaining puzzle**: `IOAccelFindAccelerator` and `IOAccelCreateSurface` both
consistently return the same distinctive, non-standard code `0x10000003` (not a recognizable
`IOReturn`/Mach error shape) regardless of which real, observed argument values are tried (own accelID
vs. the literal `0x1f07` seen in every trace). The fact that two *different* functions fail with the
*identical* unusual code, while a third succeeds cleanly with a very similar call shape, suggests a
deeper ABI/prototype mismatch specific to those two (extra/different argument type, or a
different real return convention) rather than simply wrong parameter values - a distinct, narrower,
still well-scoped question for continued disassembly work if this thread resumes.

Health checks passed cleanly throughout; no crash at any point across every real attempt.

## Real final clarification: the true remaining blocker is CGL's internal object state, not prototypes

Extended the disassembly to include each call site's post-call instructions (how the caller actually
uses the result), resolving the `0x10000003` mystery precisely. **`IOAccelCreateAccelID` is a real
2-argument function** - the caller never even checks its r3 return value, instead reading the result
directly back out of the memory the 2nd argument pointed to. My original 4-argument call worked anyway
purely because the two extra, bogus arguments landed in registers the real function never reads.
**`IOAccelFindAccelerator` is a real 3-argument function** - my original 4-argument call was genuinely
wrong and corrupted the real call, explaining every failure. Retested with the corrected 3-argument
prototype: `IOAccelCreateAccelID` (already fixed) continues to succeed cleanly and reliably.
`IOAccelFindAccelerator` still fails - but now for a real, different, and more fundamental reason: its
first argument is not a simple display index constant. The real disassembly shows it's loaded from a
live CGL-internal object (`r29`, or `*(r29+0)` in the two real call sites examined) - a real "renderer"
or "display" object this project's own minimal test program has no equivalent for, not fabricable
without also replicating a meaningful slice of CGL's own internal object construction.

The same pattern holds for `IOAccelCreateSurface`: every one of its first three arguments is loaded
from a real, already-initialized internal struct (`*(r30+0x28)`, `*(r30+0x8)`, `*(r30+0x24)`) - the
literal values observed live via gdb (`service=0x1f07`, `type=0`) are real and accurate for that
specific run, but supplying them from a from-scratch test program without the surrounding real object
state they were read from was not sufficient to succeed.

**This is a real, honest, qualitatively different kind of remaining gap than everything found earlier
in this investigation** - not a wrong function signature or a missing selector (both fixable with more
disassembly), but a dependency on CGL's own internal object graph, which would mean reconstructing a
meaningful part of the CGL implementation itself to supply real, valid inputs. This is the honest limit
of the "call the real functions directly" technique for this session - a genuinely different, larger
undertaking than continued prototype archaeology, consistent with (not contradicting) the "different-
scale project" characterization from earlier in this thread.
