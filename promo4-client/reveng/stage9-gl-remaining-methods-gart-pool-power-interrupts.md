# GL context's remaining methods, GART pool arbitration, and two clean negative results

Continuing the full autonomous surface-area sweep.

## GL context's remaining ~17 external methods: all real, all confirm existing architecture

Decompiled every previously-named-but-unread GL context method (`get_config`, `get_status`,
`set_swap_rect`, `set_swap_interval`, `set_surface_volatile_state`, `get_surface_size`,
`reclaim_resources`, `get_surface_info`, `new_texture`, `delete_texture`, `become_global_shared`,
`page_off_texture`, `scale_surface`, `purge_texture`, `get_data_buffer`, `set_stereo`, `read_buffer`).
All are real, substantive implementations - no stubs, no surprises, and every one confirms
architecture already established elsewhere this session rather than revealing anything new:

- `new_texture` dispatches on a real type tag (0/1/2/3/6/7) across `IOATIR500Shared::new_surface_texture`/
  `new_global_texture`/`new_texture`/`new_agpref_texture` - the same shared cross-context texture
  allocator already confirmed for the 2D path (`stage8-...md`).
- `get_data_buffer` implements a real, complete data-buffer pool allocator with GART-backing-store
  creation, cache-size doubling under pressure, and real free-list/LRU bookkeeping capped at 16 cached
  buffers (matching `reclaim_resources`'s identical cap).
- `read_buffer` implements real pixel-readback: clips the requested rect against live surface bounds,
  allocates a GART-visible destination buffer, and issues a real vtable-dispatched blit-to-readback call
  - the real mechanism behind `glReadPixels`, confirming this path exists independently of any of the
  embedded-marker-language machinery already fully mapped.
- `get_config`/`get_status` both do a real "flush all in-flight state to the surface's underlying
  register-tracking table" scan (up to 23 tracked units) before reading back live values - real,
  consistent with the fence/dirty-tracking architecture already understood.
- `page_off_texture`, `purge_texture`, `become_global_shared` all implement real, correct linked-list/
  reference-count bookkeeping matching the shared-texture-allocator model.

No further investigation of this cluster is warranted - it's real, working, and unsurprising.

## `IOATIR500Accelerator::freeToAllocGART`/`freeWaitToAllocGART`: the real global GART reclamation sweep

Real, substantial, now fully understood. When any context needs GART space for a new buffer and the
immediate allocation fails, this function performs a real, two-pass (gentle then aggressive) sweep
across **every live context of all four types simultaneously** - iterating every `IOATIR5002DContext`,
`IOATIR500DVDContext`, and `IOATIR500GLContext` instance's own `freeToAllocGART` and its associated
`IOATIR500Shared` allocator, then every live `IOATIR500Surface`, then the cached free-transfer-buffer
ring pools (two separate power-of-two ring buffers at `this+0x400`/`this+0x5c4`), then finally the
specific caller-associated contexts/buffer. **Real, concrete implication for a from-scratch PROMO4
client**: GART space is a single, global, cooperatively-reclaimed pool shared across every context in
the system, not per-context-isolated - a client's own transfer buffer can be evicted by GART pressure
from *any* other context (2D, DVD, GL, or Surface) system-wide, not just its own activity.

## Two clean negative results: no custom power management, no real hardware interrupt handling

Checked the kext's memory-block map (`__text` vs. the synthetic `EXTERNAL` placeholder block,
`0x4e000`-`0x6adff`) against every power-management and generic-interrupt-controller symbol found:
`setPowerState`, `registerPowerDriver`, `powerStateWillChangeTo`, `powerStateDidChangeTo`,
`acknowledgeSetPowerState`, `PMinit`, `PMstop`, and every generic `*Interrupt*` IOKit method
(`causeInterrupt`, `enableInterrupt`, `registerInterrupt`, etc.) **all resolve to addresses inside the
synthetic `EXTERNAL` block** - none of them are real, kext-implemented functions. **This accelerator
kext implements no custom power-management logic at all**, and **is not itself an interrupt controller**
- both rely entirely on IOKit's default/inherited behavior (or, more likely, real power-management for
this GPU lives in a separate framebuffer/display kext this project has never downloaded).

The two real `IOInterruptEventSource`-shaped functions that *do* exist as genuine kext code
(`IOATIR500Accelerator::garbage_collector` and `::gart_collector`, both real addresses well outside the
`EXTERNAL` range) are, on inspection, **pure software deferred-work callbacks** - `garbage_collector`
frees orphaned textures, `gart_collector` frees GART wirings when free space drops below a threshold.
Neither touches any MMIO register or GPU interrupt-status bit. **Definitive conclusion: this GPU's
command-completion detection is polling-only** (matching everything already known about the fence/stamp
mechanism) - there is no real GPU hardware interrupt path anywhere in this kext.

## 2D and DVD contexts each implement their own distinct, extensive embedded-opcode command language

Both `ATIR5002DContext::process_command_buffer` (`0x326d0`) and `ATIR500DVDContext::process_command_buffer`
(`0x357c0`) exist (a `ATIR500Surface::process_command_buffer` does *not* - Surface is confirmed to never
be a command-stream consumer, purely a backing-store/lock bookkeeping class). Both real command
processors use the **identical structural mechanism** already fully mapped for the GL context (top-byte
opcode dispatch over the same `0x02`-`0x47`-ish range, the same "patch header to `0x80000000`/`0x1150`-
style Type-3 NOP once consumed" convention, the same deferred texture-load-and-patch pattern) - this is
now confirmed to be a truly generic, shared mechanism across every context type in this driver, not a
GL-specific design. The specific opcode *numbers* and field layouts are however genuinely different per
context (each context implements its own numbering for conceptually similar operations, e.g. 2D's
texture-bind opcodes sit at different top-byte values than GL's).

**A deliberate scoping decision, given the scale**: both command languages are large enough (2D's spans
the full `0x02`-`0x47` range with ~45 distinct branches; DVD's is similarly large, with real
double-precision float math) that fully tracing either to the same level of detail as the GL command
language (`stage4-embedded-opcode-table-completed.md` + its two follow-ups) would be a comparably-sized
undertaking on its own. Given 2D/QuickDraw acceleration is peripheral to this project's actual H.264/3D-
decode goal, its language was only structurally confirmed, not traced further. **DVD's command language
is judged the single most valuable remaining follow-up target for a future session** given what a brief
sample already shows: opcode `0xa000000` computes real **planar YUV 4:2:0 chroma-plane geometry**
(`plane_height * pitch * 3 >> 1`, the standard combined-luma+chroma-plane size formula for 4:2:0 video),
and opcode `0xd000000` builds what looks like a real texture-sampler-state record (header `0x1393`,
count `10`, literal constants `0xc0069a00`/`0x52f036da`) - direct, concrete evidence supporting
`stage8-dvd-overlay-and-2d-acceleration-decoded.md`'s hypothesis that decoded video reaches the screen
via a textured-quad blit rather than a hardware overlay plane, and a real, promising thread for whoever
picks this project back up with H.264 presentation specifically in mind.

## Honest limits

- 2D's and DVD's embedded-opcode languages are structurally confirmed and lightly sampled, not
  exhaustively traced - a deliberate scoping decision given their scale, documented above with reasoning
  rather than left as a silent gap.
- Nothing in this document was run on real hardware - pure static analysis, per the standing constraint
  for this session (no G5 access).
