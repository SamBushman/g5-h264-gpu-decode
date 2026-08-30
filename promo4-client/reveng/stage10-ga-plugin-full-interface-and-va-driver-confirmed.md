# GA plugin's complete real interface, Surface class confirmed, VA driver confirmed - closing the sweep

Final section of the full autonomous surface-area sweep. Covers the last three items: the GA plugin's
"cursor/VBL" entry points (the original survey's guess about their existence was wrong, but investigating
turned up something better), the Surface class's lock/shape family, and the VA driver's renderer object.

## Correction: there is no cursor handling in the GA plugin - but a real, complete 19-method interface exists

The original survey guessed at "cursor handling, VBL sync, other blit variants" as unexplored GA plugin
surface. Checking the plugin's complete real symbol table (207 symbols total, only 8 are real non-import
functions) shows **no cursor-related function exists anywhere in this binary** - that guess was wrong.
What actually exists, and had never been read, is `_radeonGAInterface` (`0xe544`) - not a function at
all (it lives in `__data`, not `__text`; the earlier "bad instruction data" from trying to decompile it
was Ghidra attempting to disassemble a data structure). It's the plugin's real CFPlugIn interface vtable,
a 19-entry function-pointer array:

`QueryInterface`/`AddRef`/`Release` (standard `IUnknown`), then `Probe`, `Start`, `Stop`, `Reset`,
`GetCapabilities`, `Flush`, `Synchronize`, **`GetBeamPosition`** (this *is* the real VBL-adjacent
functionality the original survey was gesturing at, just accessed as one interface method rather than a
dedicated pair), and a full `AllocateSurface`/`FreeSurface`/`LockSurface`/`UnlockSurface`/`SwapSurface`
family.

Decoded the newly-found ones:

- **`GetBeamPosition`**: calls the bound 2D-context connection's external method **selector `0x10`**
  (`read_regs`, per `stage8-...md`'s table) to read a raw register, then extracts bits `[26:16]` (11
  bits) as the scanline position - a real, concrete, independently-confirmed usage example of
  `read_regs` for exactly its intended purpose (a small, safe, kernel-validated register read), and a
  second full validation that `stage5`/`stage8`'s decoded table entries are correct.
- **`AllocateSurface`**: large and substantial - dispatches on flag bits to either call the bound 2D
  context's `create_image`(selector `9`)/`declare_image`(selector `8`)/`create_transfer`(selector `10`)
  external methods (all three call sites match `stage8`'s decoded table exactly, a third independent
  confirmation) or directly construct raw embedded command-buffer content itself, including a literal
  header value **`0x1393`, count `10`** - the *exact same* literal pair already seen, independently, in
  the kernel's `ATIR500DVDContext::process_command_buffer` sample in `stage9-...md`. This is a real,
  concrete, cross-binary consistency confirmation that userspace code and the kernel's own command
  processor share this exact record format, though the record's full field-by-field meaning was not
  further resolved this pass (an honest limit, not a contradiction).
- **`LockSurface`/`SwapSurface`/`Flush`**: straightforward real wrappers around the 2D context's
  `lock_memory`(selector `5`)/`swap_surface`(selector `3`) external methods and the shared-memory
  command-buffer cursor/chain-link setup already fully understood from the kernel side - genuine,
  working userspace counterparts to the kernel API this project has spent the session mapping, not new
  mechanisms.
- **`_radeonCopyRegion`** (the other previously-unexplored real function, `0x55a0`): a large, real
  multi-rectangle region-copy composer (handling exposed-region window updates with per-rectangle
  clipping) that calls `_radeon3DCopySetup` **eight separate times** - a third independent real-code
  confirmation of that already-fully-understood register sequence (after the original `_radeon3DCopySetup`/
  `_radeon3DFillSetup` pair and `radeonCopy`/`Fill`/`Highlight`/`SolidScanlines`).

**The GA plugin's real API surface is now completely accounted for**: 8 real functions plus the
19-method interface struct, everything either fully decoded or confirmed to reuse already-understood
mechanisms.

## Surface class's lock/shape family: confirmed standard, no surprises

Sampled `surface_control`, `surface_flush`, `get_state`, `set_shape`, `surface_read`,
`surface_query_lock`, and a lock/unlock pair. All real, all exactly what their names suggest - standard
IOSurface-style locking with real pending-flush detection, real on-demand surface allocation via
`alloc_surfaces_retry`, and (in `surface_read`) the identical clipped-readback pattern already seen in
the GL context's `read_buffer`. This closes the Surface class as fully, if lightly, covered: it is
exactly what earlier analysis characterized it as - window-server shared-surface bookkeeping, peripheral
to the compute-acceleration goal, with nothing unexpected in its real implementation.

## VA driver's real renderer object: confirmed, not re-litigated

`_AVACreateRenderer`/`_AVACreateRendererDisplayExt`/`_AVACreateRendererDVDExt`/`_AVAGetRendererInfo` are
real, standard COM/CFPlugin-style object factories, building real 22-slot (main) and 3-slot (DVD
extension) function-pointer vtables, string-tagged `"ATIVADriver "`. Real `cos`/`sin` imports and a
substantial float/double constant table are consistent with real geometric/color-matrix transform work
(rotation, colorspace conversion) rather than anything decode-related. **This confirms, rather than
overturns, `stage3-va-driver-is-not-a-decode-shortcut.md`'s original conclusion** - a real, working
overlay/colorspace-conversion renderer, not an H.264 decode path. The internal per-method dispatch
(a large switch statement, ~28 real cases) was not exhaustively traced, given the factory-level
confirmation already settles the question this project needed answered from this binary.

## Closing note: the full driver surface-area sweep is complete

Between this document and `stage7`/`stage8`/`stage9`, every item identified in the "what other surface
area is left to explore" survey has been addressed: all three tooling ceilings resolved, `IOServiceOpen`
type values resolved for 2 of 4 (with the other 2 shown to be genuinely outside this project's downloaded
binaries), every named-but-unread external method across all four kext context classes decoded or
confirmed as a stub, GART pool arbitration fully understood, power management and interrupt handling
both definitively shown to not exist in this kext, the 2D and DVD contexts' own embedded command
languages structurally confirmed (with DVD's flagged as the best remaining future target given its
direct YUV/video relevance), and the GA plugin and VA driver bundles' complete real API surfaces
accounted for. Nothing here was run on real hardware, per the standing constraint for this session.
