# Top-down synthesis: a fully-grounded PROMO4 redesign proposal

This closes out the autonomous decompilation sweep (`stage4-full-driver-sweep.md` and its companion
docs) with the top-down deliverable it was building toward: given everything learned bottom-up across
every downloaded driver binary, what does a well-grounded next attempt at PROMO4's original goal
(native-ISA shader execution via an independent client) actually look like now? Per the user's standing
instruction for this whole sweep: **this is a proposal and a set of testable hypotheses with their test
methods - nothing here has been attempted, and nothing should be attempted without the G5 back up and a
fresh, explicit go-ahead.**

## What changed, bottom-up, that makes a new attempt better-grounded than the last one

1. **The exact root cause of both hangs is now proven, not inferred** - `stage3g-cursor-field-
   misidentified.md` (wrong field) and `stage3-kernel-side-hang-mechanism-confirmed.md` (the kernel's
   chain-walk has zero bounds checking, confirmed identically in both `ATIR500GLContext` and
   `ATIR5002DContext`). The real write cursor is `AGLContext+0x17dc`, not `+0x17d8`.
2. **A complete, real, Apple-authored register sequence for minimal 3D pipeline bring-up now exists**
   (`stage4-radeon3DCopySetup-complete-draw-reference.md`) - not a third-party reconstruction, Apple's
   own shipped `ATIRadeonX1000GA.plugin` code, confirmed register-by-register, confirmed independently
   TWICE (copy and fill variants) and again via `radeonCopy`/`radeonFill`/`radeonHighlight`/
   `radeonSolidScanlines`.
3. **This project's own shader-ISA encoding is now independently validated from Apple's own binary**,
   not just the AMD spec read by hand - the literal instruction word `0x20490000`
   (`US_ALU_RGBA_INST_VAL`) appears in Apple's own code, and `GA_US_VECTOR_INDEX`/`DATA`/`US_CODE_ADDR`
   are the exact registers Apple's own 2D-via-3D path uses.
4. **A real, proper synchronization primitive exists and is now understood end-to-end**
   (`stage3-fence-mechanism.md` + this sweep's `submit_buffer_retired`/`waitForRetiredTimeStamp`
   decoding) - a real fence snapshot, a real kernel wait (external-method selector `9`), and
   confirmation the completion counter is DMA'd system memory, not a simple register poll.
5. **HyperZ/Z-buffer setup is confirmed skippable for a minimal test**, not just by register default but
   by Apple's own driver logic (`compute_sc_hyperz_en`/`compute_zb_bw_cntl`, decoded directly from the
   kext in this sweep).
6. **The embedded "extended opcode" marker language is now understood to have zero input validation
   anywhere it's used** - real, systemic, confirmed in two separate context classes. Any design that
   avoids this language entirely (staying on the "plain path" `stage3g-cursor-field-misidentified.md`
   already found reaches a clean, header-confirmed state) sidesteps this entire risk category, not just
   works around one specific bug in it.

## The proposal: a minimal, fully-sourced draw, staying off the marker-language path entirely

**Hypothesis**: a hand-built client can draw a real triangle/quad to an isolated `AGLContext`'s pbuffer
by (a) never touching the chain-link field at `+0x17d8`, (b) writing only to the confirmed real cursor
at `+0x17dc`, (c) using the exact register sequence `_radeon3DCopySetup`/`_radeon3DFillSetup` already
prove work on real hardware (substituting this test's own framebuffer address/format), and (d)
synchronizing via the real fence mechanism instead of an immediate follow-up GL call - and that doing
so will not hang the machine, because it never exercises the unbounded chain-walk at all.

**Concrete design** (documentation only, not code to run):
1. Create an isolated `AGLContext`+pbuffer exactly as `stage3_native_shader.c`/`stage3g_real_injection.c`
   already do (proven safe pattern, repeated across many runs with zero hangs when no raw injection is
   involved).
2. Force the buffer onto the "plain path" the same way `stage3g`'s `settle_plain()` already does
   (unbind any program, one plain clear+finish) - already verified, read-only and live, to reach a
   clean, header-confirmed state (`+0x1c == 1`, cursor at a real, header-relative offset).
3. Read the live cursor at `AGLContext+0x17dc` (the corrected field) and verify it against the base
   (`+0x17e4`) and the real end/limit field (`+0x17e0`, identified but never used by this project's own
   code yet) - abort if anything looks unverified, exactly like the existing `inject_at_live_cursor`
   safety check, just pointed at the right field this time.
4. Write the real `_radeon3DCopySetup`/`_radeon3DFillSetup` register sequence (verbatim structure,
   substituting this test's real framebuffer address, format, and dimensions) starting at that cursor -
   plain Type-0 register writes only, no embedded opcode markers, no chain-link interaction.
5. Advance the (correct) cursor field by the real number of dwords written.
6. Trigger the real submission the SAME way the client already reliably does (a plain `glFinish()`),
   since this doesn't touch the marker-language path either.
7. **Synchronize properly**: rather than immediately reading pixels back or making another GL call,
   snapshot the buffer's own tag (`bufferBase+0x18`) before submitting, then either poll the live
   completion counter this sweep found (`AGLContext`-relative equivalent of the kernel's
   `this+0x864/0x868` fields - not yet mapped to an `AGLContext` offset, a real remaining gap) or call
   the existing fence API (`_gldCreateFence`/`_gldFinishObject`) directly, before reading results.
8. Read back the pbuffer and compare against the expected fill/copy color - the same verification
   pattern every prior stage in this project has used.

## Update: custom shaders no longer require writing a compiler

`stage6-shader-compiler-architecture-and-strategic-shortcut.md` found that `libGLProgrammability.dylib`
is a real, public, directly-linkable framework exposing a complete ARB-assembly/GLSL front-end parser
plus a full optimizing middle-end (inlining, loop unrolling, constant folding, dead-code elimination,
and real interference-graph register allocation) - the same one the real ATI driver bundle calls. Any
future extension of this proposal to *custom*, non-fixed shaders should link this framework directly
(`_glpPPShaderToProgram` then `_glpPPShaderLinearize`) rather than writing a compiler from scratch; only
a much smaller final "generic IR to R5xx instruction word" encoder remains genuinely new work, and its
target format (the `US_ALU_RGBA_INST`-style encoding) is already independently confirmed in
`stage4-radeon3DCopySetup-complete-draw-reference.md`.

## Update: an even more complete, more directly Apple-sourced register map now exists

`stage4-complete-register-tracking-state-map.md` (found by tracing this same function family's real
caller, `restore_state_destroyed_by_pageoff`) supersedes the KolibriOS-derived sequence step 4 above
cites - it's Apple's own "rebuild the entire render state after a VRAM eviction" function, which by
construction must cover every register that matters, confirmed field-by-field against a real struct
rather than reconstructed from a third-party driver. Any future implementation of step 4 should use
that map as the primary register-value reference, with the KolibriOS sequence as a secondary
cross-check where the two overlap (they agree everywhere they've been compared so far).

## Real, honest remaining gaps this proposal doesn't close

- **The client-side `AGLContext`-relative offset for the kernel's own completion-counter base
  (`this+0x864`) was never mapped this sweep** - the fence mechanism's `+0x1838` field is confirmed
  from the GL driver bundle side, but connecting it precisely to the kernel's own `waitForRetiredTimeStamp`
  base pointer wasn't completed. Not required for the proposal above (step 7 can use the existing,
  already-proven `_gldFinishObject` call instead of a raw poll), but would be worth closing if a future
  session wants a fully from-scratch synchronization primitive.
- **The real end/limit field (`AGLContext+0x17e0`) has never been read by any of this project's own
  test programs** - `stage3g_real_injection.c`'s safety check only ever compared against the header
  size, not the buffer's real remaining capacity. A future version should read and honor it.
- **This proposal has never been run.** Every register value, offset, and mechanism it cites is
  independently confirmed by decompilation and cross-referencing, but confirmation-by-static-analysis
  is not confirmation-by-execution. The next real step, whenever the G5 is back and a fresh go-ahead is
  given, is exactly this design, built and run with the same incremental caution (read-only diagnostics
  first, then a single conservative write, verified before any second write) this project has used
  every other time it has approached this machine.

## Test method summary (for the record, not for this session)

Build a new, small standalone test program (following the existing `stage3g`/`stage3h` naming and
structure) implementing the eight steps above. Run it exactly once, observe whether a fresh SSH
connection remains responsive throughout and after, and whether the pbuffer readback matches the
expected fill color. If it hangs, the exact same "stop immediately, no further remote attempts,
physical power-cycle required" protocol this project has followed twice already applies without
exception.
