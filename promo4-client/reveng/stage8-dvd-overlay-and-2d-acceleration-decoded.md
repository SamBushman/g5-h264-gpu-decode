# DVD overlay path and 2D acceleration primitives, fully decoded

Continuing the full autonomous surface-area sweep. This covers the two richest previously-named-but-
unread method clusters: the DVD context's real overlay control, and the 2D context's real acceleration
primitives.

## DVD overlay path: mostly vestigial in this exact kext build

All five DVD-context wrapper methods (`show_buffer`, `dvd_setup_overlay`, `dvd_enable_overlay`,
`dvd_setup_subpicture`, `dvd_enable_deint`) are thin locking wrappers that validate the device-active
flag and a bound-surface pointer, then delegate to the real implementation on `ATIR500Surface`. Reading
those real implementations:

| Real `ATIR500Surface::` method | Real behavior |
|---|---|
| `disable_overlay()` | **Empty no-op.** |
| `enable_overlay()` | **Empty no-op.** |
| `showbuffer(int, int)` | **Empty no-op.** |
| `dvd_setup_subpicture(int,int,int,int)` | **Empty no-op.** |
| `dvd_setup_overlay(int,int,int,int)` | Real: stores x/y/w/h geometry into `this+0x94/0x96/0x98/0x9a` (the *same* fields `setup_buffers`, already decoded, writes via the DVDContext wrapper - confirms one shared geometry record) and sets a dirty/enable flag at `this+0xd94`. |
| `enable_deint(int)` | Real: stores the deinterlace mode into `this+0xdac`. Nothing reads or acts on it in any function traced this session. |

**This is a genuine, significant, unexpected finding**: the actual on-screen presentation/enable/disable
of a hardware video overlay plane does *nothing* in this driver build. Only the geometry and deinterlace-
mode *bookkeeping* is real; the functions that would actually turn a hardware overlay on, show a new
buffer, or configure subpicture compositing are stubs. Combined with `stage4-real-hardware-idct-engine-
found.md`'s independent-ring IDCT discovery and this project's existing `_radeon3DCopySetup`/
`_radeon3DFillSetup`/opcode-`0x31` findings (real, working "blit via textured 3D quad" techniques used
throughout this driver generation for 2D fill/copy *and* FSAA resolve), the most consistent explanation
is: **this GPU generation presents decoded video the same way it presents everything else - as an
ordinary texture composited via a 3D draw, not through a dedicated hardware overlay plane.** The overlay
API surface here reads as inherited/vestigial scaffolding from an earlier chip generation that actually
had a working overlay engine, never removed, but never wired up for R5xx. **Real implication for
PROMO4's H.264 goal**: don't plan around a hardware overlay path at all - the IDCT engine's output
should be expected to become an ordinary GPU surface/texture, then presented via the same textured-quad-
blit technique already fully understood and confirmed working (Apple's own code, twice) elsewhere in
this project.

## 2D acceleration primitives: real, substantial, and share the GL texture allocator

- **`create_image`/`declare_image`**: both allocate through `IOATIR500Shared::new_texture`/
  `new_agp_texture` - **the same shared, cross-context texture-allocation object** GL contexts use
  (`IOATIR500Shared`, lazily created via `create_shared` on first use). Real, useful architectural
  confirmation: 2D and 3D acceleration are not separate texture-memory worlds in this driver - they
  share one real allocator.
- **`create_transfer`**: allocates a real AGP-backed transfer buffer (`new_agp_texture`) and, if a
  surface is currently bound, performs a real backing-store swap (`free_buffer_backing_store` then
  `attach_buffer_backing_store` with a real 128-byte-aligned pitch computation) - a genuine double-
  buffer/DMA-transfer mechanism, not a stub.
- **`delete_image`**: real bounds-checked lookup and reference-counted cleanup, correctly detaching any
  surface backing store still pointing at the freed texture before calling `IOATIR500Shared::
  delete_texture`.
- **`wait_image`**: real fence wait - looks up the target texture's buffer, calls a vtable method at
  offset `0x550` (the same completion-counter/retirement-count bookkeeping pattern - `this+0x75c`
  accumulates a running total - already seen governing the GL side's fence mechanism), confirming the
  2D path uses the identical real synchronization primitive as GL, not a separate one.
- **`lock_memory`/`unlock_memory`/`swap_surface`**: all three are real, non-trivial retry loops (up to
  1000 iterations, then one final forced attempt) that call `IOATIR500Surface::alloc_surfaces` on demand
  if the target surface isn't yet backed, and use real per-surface state flags to detect pending
  GPU-side flushes before granting a CPU lock - genuine, working surface-locking logic, not a stub.
  `unlock_memory` triggers a real `swap_surface` call for negative lock-type values - the real
  present/flip mechanism for this 2D path.
- **`set_surface_paging_options`/`set_surface_vsync_options`**: **both unconditionally return
  `0xe00002c7`** (a real, deliberate "unsupported" error code, not a crash or silent no-op) - genuinely
  unimplemented in this build, a second confirmed instance of the same "vestigial API surface" pattern
  seen in the overlay path.
- **`set_macrovision`**: real and functioning - iterates every active display-connection object and
  calls a vtable method (opcode `0x92`) on each, a real, working macrovision copy-protection-signaling
  implementation. Not a stub - copy-protection signaling is genuinely wired up even though overlay/vsync
  paging control are not.

## Honest limits

Nothing here was run on real hardware - pure static analysis of the local kext binary, per the standing
constraint for this session (no G5 access). The "vestigial overlay" conclusion is an architectural
inference from decompiled code, not confirmed by observing real video playback - a future session with
hardware could verify directly (e.g., a live `gdb` watchpoint on `ATIR500Surface+0xd94`/`+0x94` during
real DVD Player playback, matching the same live-tracing method already proposed for the IDCT engine).
