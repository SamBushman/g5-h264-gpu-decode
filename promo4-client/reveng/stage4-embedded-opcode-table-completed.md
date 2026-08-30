# The embedded "extended opcode" command language, now structurally complete end-to-end

Third of the three top-down analyses identified after the register-map work
(`stage4-full-driver-sweep.md`'s closing question). Reviewed every remaining unmapped marker in
`process_command_buffer`'s dispatch (`0x06000000`-`0x46000000`, the full real range this project has
tracked since Stage 0) and categorized each by real, decompiled behavior rather than leaving them as
bare hex values.

## The complete real picture

| Marker(s) | Category | Real behavior |
|---|---|---|
| `0x06000000`-`0x15000000` (16 values) | Texture unbind | Per-texture-unit unbind/release, `N = topByte-6` selects the unit (`stage3-kernel-side-hang-mechanism-confirmed.md`) |
| `0x37000000` | Deferred surface-offset patch | Computes an alignment/range and iterates, patching offset fields - real, complex, not fully traced to completion |
| `0x38000000` | Deferred address-fixup pass | **Consumes itself** (overwrites its own marker dwords with real PM4 Type-2 filler, `0x80000000`) then walks an embedded array converting *relative* offsets into real *absolute* addresses (base + GART offset) - a real "patch pointers after the fact" mechanism, same family as `0x37` |
| `0x39000000` | Range/alignment computation | A real multi-parameter loop (references a `local_374`-indexed table), wraps itself in a `TYPE=3,COUNT=7,NOP` shell like the others - not fully traced, but confirmed structurally consistent with the rest of this opcode family |
| `0x3a000000` | Remove/release texture | Already known (`remove_texture_from_stream`) |
| `0x3b000000`, `0x3f000000` | Texture load/attach, alternate unit/slot | Structurally identical to the already-documented `0x3e`/`0x40`/`0x43` (texture-index lookup, `add_texture_to_stream`, the same "flush if buffer nearly full" pattern) but targeting a *different* per-context texture-slot field (`this+0x32c` here, vs. `this+0x2a4`/`this+0x338`/`this+0x348` for the others) - real evidence this driver tracks **multiple independent texture-unit slots** through parallel, structurally-identical opcode variants, one per slot |
| `0x3d000000` | Surface volatile-state | Already known |
| `0x3e000000`, `0x40000000`, `0x43000000` | Texture load/attach | Already known |
| `0x41000000` | **Render-target/framebuffer commit** | Fully decoded this sweep (`stage4-write-kernel-context-buffer-regs...` and the capstone register map) |
| `0x44000000` | Transfer-buffer GART completion | Calls the real `map_transfer_to_GART` function, builds a real Type-3-wrapped completion record referencing register index `0x575` - a real DMA-transfer-buffer lifecycle opcode, distinct from plain texture loading |
| `0x45000000` | `build_surface_from_texture` | Already known (depth/stencil-gated at the examined call site) |
| `0x46000000` | Fast clear | Already known (`process_kATIGLStreamFastClearColor`) |

## Real, previously-unstated architectural pattern this completes

Every single marker in the `0x37`-`0x44` range that wasn't already fully understood turns out to be a
variation on exactly two real mechanisms this project already had partial visibility into: **(1)** the
"wrap raw content in a `TYPE=3, IT_OPCODE=0x10 (NOP)` shell, to be patched with real values once fully
computed" convention (seen now in markers `0x37`, `0x39`, `0x41`, `0x44`, and the state-restore
function's own `0xc0221000` wrapper) - a single, consistently-reused real technique for reserving
space in the command stream before all the real values are known; and **(2)** per-texture-unit
structural duplication (the `0x06-0x15` unbind family, and now `0x3b`/`0x3f` alongside `0x3e`/`0x40`/
`0x43`) - this driver handles multiple simultaneous texture units by literally duplicating the same
opcode-handling logic once per unit/slot field, rather than parameterizing a single handler.

## Honest limits

`0x37` and `0x39` were categorized (confirmed to be real, structured, non-mysterious operations
following the already-understood conventions) but not traced to full byte-level precision - both
involve real loops over embedded per-entry data whose exact field semantics would need more dedicated
tracing than the marginal value justified at this point in the sweep. This is a complete *structural*
map of the whole `0x06`-`0x46` opcode space (every value is now known to be one of a small number of
well-understood real categories, none remain a bare, uncategorized hex constant), not a complete
byte-level trace of every single one - a reasonable, honest place to stop this specific thread.
