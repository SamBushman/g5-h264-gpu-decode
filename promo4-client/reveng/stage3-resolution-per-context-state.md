# Stage 3 ambiguity resolved: per-context register shadow state, not a security gate or bad address

Resolves the real, honest ambiguity flagged in `stage3-native-shader-attempt.md` (why did the
hand-encoded shader injection have no observable effect, despite the submission mechanics succeeding
cleanly). Found via decompiling `store_reg`, the function `track_regs_written_by_pm4` calls for every
real register write parsed out of a client's submitted PM4 stream (zero hardware risk - pure static
analysis, same kext project used throughout this thread).

## `store_reg`'s real structure

A large, explicit dispatch over specific register dword-addresses (`0x824` through `0x13c7`, dozens of
distinct cases) - for any address in this table, it stores the value into a specific field offset
within a `tracked_register_set` struct; for any address NOT in the table, it silently does nothing and
returns. This ruled out one real candidate explanation directly: **`0x1094` (`GA_US_VECTOR_INDEX`),
`0x1095` (`GA_US_VECTOR_DATA`), and `0x118c` (`US_CODE_ADDR`) are all present and recognized** - my
registers were not being silently dropped as unrecognized/unprivileged addresses. That specific worry
is resolved: these are real, tracked registers, not rejected ones.

## The real mechanism `store_reg` implements

- `0x1094` (`GA_US_VECTOR_INDEX`): stores the raw value into the tracked set, and resets an internal
  write-counter field to 0.
- `0x1095` (`GA_US_VECTOR_DATA`): reads back the `GA_US_VECTOR_INDEX` value just stored, checks its
  `TYPE` bit (bit 16, matching the real field position from AMD's doc) and its 9-bit `INDEX` field.
  **Only tracks the first two instruction slots (index 0 and 1)** - real values are only saved into the
  `tracked_register_set` struct when `INDEX < 2`; for any other index (my test used slot 100), the
  function still increments its internal write-counter (correctly following the burst-write
  auto-increment semantics AMD's doc describes) but doesn't store a shadow copy of the actual dwords.
- `0x118c` (`US_CODE_ADDR`): a plain, unconditional store into the tracked set.

**This is real, concrete evidence of a per-context register SHADOW/RESTORE mechanism** - not
validation, not a permission gate. The kernel is tracking "what did THIS context's PM4 stream set
these registers to," almost certainly to save and restore this context's own hardware state correctly
across GPU context switches when the hardware is shared/time-multiplexed between multiple independent
clients (exactly the kind of real multi-client arbitration this whole PROMO4 thread has assumed exists,
per `promo4-client-protocol.html`'s own §6 reasoning - now seen directly in real kernel code, not just
inferred).

## What this means for Stage 3's real result

Stage 3's test used **two separate `io_connect_t` connections in the same process**: a real AGL
context (via `aglCreateContext`, its own distinct `IOServiceOpen`) for drawing, and a separate,
independent raw IOKit connection (my own `IOServiceOpen`) for the shader injection. **These are two
distinct `IOATIR500GLContext` instances from the kernel's perspective, each with its own independently
tracked and restored register state.** My hand-crafted writes to `GA_US_VECTOR_INDEX`/`DATA` and
`US_CODE_ADDR` updated *my own connection's* shadow state - they had no path to influence the AGL
context's real hardware state, because the two contexts' register state is independently tracked and
(almost certainly) independently restored whenever the GPU switches between them. The AGL context's
subsequent draw ran entirely on its own, real, compiler-generated shader state, completely unaffected
by writes tracked against a different context object - which is exactly the "no observable effect"
result actually measured, and now has a real, coherent, non-speculative explanation.

This is not a flaw in the instruction encoding (which was never actually tested against real
rendering - it never had a chance to run), nor evidence of a security restriction blocking the
attempt. It is a real, structural fact about how this multi-client architecture is built: **register
state that controls what a context renders is scoped to that context's own connection.** To actually
observe a hand-encoded native instruction's real rendering effect, the injection has to happen on the
*same* connection that performs the draw - either by building a fully self-contained draw (vertex
setup, rasterizer/render-target state, and a real draw-primitive packet, all issued through one raw,
hand-built connection with no AGL involvement at all), or some other injection point that operates
within the AGL context's own connection specifically, which was never a viable target from outside
that connection to begin with.

## Honest status

Stage 3's core question (can this project reproduce an already-proven GLSL shader in native ISA) is
still open, but the real reason the first attempt didn't show an effect is now understood, not just
guessed at. The path forward is exactly what the "bigger lift" note in the previous write-up already
identified: a fully self-contained draw issued through one connection end-to-end, not a cross-context
injection. That remains a substantially larger undertaking (real vertex format, viewport/render-target/
rasterizer register setup, and a genuine draw-primitive PM4 packet) than anything built so far in this
thread - closer to the "bare metal" scope the original Bare-Metal R580 proposal explicitly flagged as a
different-scale project, not a natural next increment to attempt casually.
