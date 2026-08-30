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

## Follow-up: `0x37` and `0x39` fully traced (at the user's request)

Both opcodes were read in complete, uninterrupted context this pass. Both resolved into real,
architecturally significant mechanisms - not just "another texture load," genuinely new information.

### `0x39000000`: the real "bind vertex attribute buffers + index buffer" opcode

Full trace confirms this processes a real list of vertex-array (attribute) buffer bindings using
**the exact same texture-load machinery already documented for fragment textures** (real lookup in
the texture table at `this+0x88`, `add_texture_to_stream`, `alloc_and_load_texture`, the same
"flush if buffer nearly full" pattern) - but storing the resulting bound-buffer pointers into
per-context slots starting at **index 16** (`this+0x40*4+0x2a4`), a separate range from the `0-15`
fragment-texture-unit slots already confirmed by the `0x06-0x15` unbind family. **This is real,
direct evidence that R5xx fetches vertex attributes through the same texture-fetch-unit hardware used
for fragment textures** - a genuine, confirmed architectural fact about this GPU generation, not
previously documented anywhere in this project.

After binding, the function computes real per-attribute GPU-visible addresses via `GetVertexArrayOffset`
(already-known function, now seen in full context) - or, for a special sentinel value (`0xffffffff`),
computes a GART-relative offset directly using the identical formula seen in the deferred-patch opcodes
(`base + GART_offset + (relative - header) + 0x20`). These addresses get written back into the
command stream's own embedded slots - the same "patch it here, once it's known" convention as every
other deferred-binding opcode. A final section computes and patches a real index-buffer (element array)
offset, including a real sub-dword alignment correction (`if not 4-byte-aligned and using 16-bit
indices, bump by one`).

**Why this matters for the PROMO4 redesign**: this confirms, directly, that any draw using a real
vertex *buffer* (as opposed to the immediate-mode `3D_DRAW_IMMD_2` packet this project's own hand-built
tests already use, which embeds vertex data directly and needs none of this) would have to go through
this exact opcode and its whole deferred-binding machinery. Staying on immediate-mode draws, as the
existing redesign proposal already does, is confirmed to be the right choice for avoiding this entire
subsystem.

### `0x37000000`: the real "deferred texture/render-target offset-and-format patch" opcode

Full trace resolves the specific mechanism behind a question this project has circled since the
original Stage 3 "no scratch render-target memory" investigation: **how does the client embed a
texture/render-target reference before the kernel has finalized where that surface actually lives in
memory?** Answer, now confirmed: it doesn't - it embeds *placeholder* slots plus this marker, and the
kernel patches in the real values once they're known.

Real logic, by branch:
- Reads the currently-bound texture/render-target reference (`this+0x2a4`) and checks its real backing
  type (the same byte `GetTextureOffset` already showed distinguishes VRAM-direct/GART/surface-backed
  storage).
- **Surface-backed case**: looks up the real surface buffer via `IOATIR500Surface::surface_buffer_idx_mask`
  (already-known real function), then computes and patches real per-mip-level offset+format-control
  values directly into pre-reserved command-stream slots, using the same format-table bit-extraction
  (`DAT_0004d2dc`/`DAT_0004d2e0`) already partially decoded via `write_r500_3d_blit_state_packet`.
- **Plain-texture case**: calls the already-confirmed `GetTextureOffset` directly and does the
  identical "add the now-known base address to a relative placeholder" patch already seen in opcode
  `0x38` - plus real per-surface dirty-bitmask/reference-count bookkeeping (`*(ushort*)(surface+0x28)`/
  `+0x1c`) and stamps the current buffer-generation counter (`this+0x50`) into the surface object -
  the real "mark this surface as used by generation N" mechanism already seen governing texture
  pageoff/eviction decisions elsewhere.
- Both branches end by overwriting the original marker with a real `TYPE=3, IT_OPCODE=0x10 (NOP)`
  header sized to skip exactly the content just consumed - the same self-consuming convention already
  established for the whole opcode family.
- A `no texture bound` fallback writes a simpler NOP-wrap using a different embedded count field.

## Bonus, found while reading in full context: a new opcode, and a hint of a much larger opcode space

Immediately after `0x39`'s handler sits **`0x2a000000`** - not previously catalogued. Fully readable in
context: a real GL-attachment-enum-to-internal-slot lookup table (mapping values like `1/2/3/4/7/8/10/11`
to internal indices `0/4/5/6/2/3/7/8` - very plausibly `GL_COLOR_ATTACHMENT0/1`/depth/stencil-style
binding points), followed by computing a real render-target offset+pitch/tiling value pair (same shape
as the already-confirmed `RB3D_COLOROFFSET`/`COLORPITCH` computation), and - directly confirming a
connection to already-decompiled code - **writing `this+0x358` and `this+0x354` into the command
stream verbatim** - the exact same fields `ATIR500GLContext::build_scissor` computes. This opcode
embeds a render-target-pair binding *and* the freshly-computed real scissor rectangle together.

The surrounding dispatch chain (`else if (uVar38 < 0x2a000001) { if (uVar38 != 0x1d000000) ... if
(uVar38 != 0x17000000) ...`) makes clear there is a **whole additional opcode range below `0x2a`**
(at least `0x16`-`0x29`) this project has never catalogued at all - the "20+ distinct markers" Stage 0
originally estimated understates the real total. Not pursued further this pass (genuinely new scope
beyond what was asked) - flagged clearly as a real, concrete next target if this thread continues.

## Honest limits, updated

`0x37` and `0x39` are now fully traced, not just categorized. The newly-found `0x2a` and the broader
`0x16-0x29` range - and, it turned out, `0x02-0x05` and `0x2b-0x31` alongside it - have since been fully
chased down in `stage4-opcode-range-0x02-0x31-traced.md`, including a second confirmed real example of
the "blit via textured full-screen quad" technique (`0x31`) and a clarification that not every embedded
`(index, value)` pair targets real MMIO (`0x28`'s software-internal `0x50b` field).
