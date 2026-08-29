# Stage 3 continued: the real embedded "extended opcode" command language

Direct continuation of `stage3-scope-assessment.md`'s open item (find the real texture/surface
allocation mechanism). Real, substantial finding: it's not a missing `IOConnectMapMemory` type or a
missing external-method selector - it's a rich, real, internal command language embedded *inside* the
normal command buffer stream, sitting alongside (and distinct from) raw PM4. Pure static RE, zero
hardware risk throughout this entry.

## The real mechanism

The GL driver bundle's own command-composition code (e.g. `FUN_00017310`, and the writer at GL driver
bundle offset `0x1740c` calling `build_surface_from_texture`) writes **32-bit magic marker headers**
into the command buffer at the current write cursor - values like `0x3a000000`, `0x3d000000`,
`0x3e000000`, `0x40000000`, `0x43000000`, `0x45000000`, `0x46000000` (at least 20+ distinct markers
exist across the full range `0x1000000`-`0x46000000`, found via `process_command_buffer`'s real
dispatch). These do **not** decode as valid PM4 (their bit patterns don't correspond to sensible
Type-0/1/2/3 headers) - they're a separate, driver-private encoding the kernel's
`process_command_buffer` scans for and handles specially, consuming and overwriting each record with
real PM4 Type-2 filler (`0x80000000`) once processed, before the remainder of the buffer is treated as
real hardware PM4. This resolves Stage 0's original "chain descriptor" mystery completely: the
`&DAT_00001393, 0, 0x5c8, 0x20000` record found there is exactly this same marker/boundary format,
confirmed now from the kernel side too (`puVar65[-4]=0x1393; puVar65[-3]=0; puVar65[-2]=0x5c8;
puVar65[-1]=0x20000;` appears verbatim in multiple opcode handlers, always right before a real call to
`ATIRadeonX1000::submit_buffer`).

## Real opcodes mapped so far

| Marker | Real operation |
|---|---|
| `0x3a000000` | Remove/release a bound texture (`remove_texture_from_stream`, refcounted `delete_texture`) |
| `0x3d000000` (sub-opcode `0x132`) | `IOATIR500Surface::set_volatile_state` - mark a surface volatile (double-buffer/discard semantics) |
| `0x3e000000`, `0x40000000`, `0x43000000` | Texture load/attach - looks up a texture-table index, calls `add_texture_to_stream`, `alloc_and_load_texture`, and (when a pending sub-buffer exists) the real `ATIRadeonX1000::submit_buffer` |
| `0x45000000` | Calls `build_surface_from_texture` - but the specific client call site examined is gated on `GL_DEPTH_COMPONENT` (`0x1902`, the real GL enum), i.e. this is depth/stencil-attachment-specific in that call path, not exclusively color-target setup; `build_surface_from_texture` itself is a shared utility other opcodes may also call with different real parameters |
| `0x46000000` | `process_kATIGLStreamFastClearColor` - a real, named "fast clear" operation |

Real selector `0x12` (18) also confirmed to exist (found via a separate client call site) - used for
some form of vertex/index buffer growth allocation, called with what looks like a 1-scalar-in/1-scalar-
out shape (`local_28`/`local_24`), distinct from the already-confirmed selectors 3 and 8.

## Real clarification: the `&DAT_xxxx` references are literal constants, not lookup data

Several opcode-writer functions embed values like `&DAT_000013c4`, `&DAT_00001040`, `&DAT_00001393`
into the command buffer. Dumped these addresses directly from the GL driver bundle's own memory - **all
are zero-initialized BSS locations**. The code uses `&DAT_xxxx` (address-of) purely as a compiler/
decompiler artifact for embedding the small integer constant matching that address's own numeric value
(e.g. `&DAT_00001393` really just means the literal `0x00001393`) - not a reference to meaningful table
data. This removes a real source of uncertainty: nothing hidden needs decoding for these fields.

## Real, non-trivial finding: `process_kATIGLStreamFastClearColor` needs a properly bound surface

Full decompile shows it reads real, non-trivial per-surface state (pitch, tiling shift, a byte "format"
field at surface+0x38) and combines it with a **real, format-indexed 28-byte-stride lookup table**
(`DAT_0004d2dc`/`DAT_0004d2e0`, also used independently by `write_r500_3d_blit_state_packet`) to compute
the real clear-color register packet. **Dumped this table directly** (40 real entries, 28 bytes each,
starting at GL-driver-bundle offset `0x0004d2dc`) - confirmed real, structured, non-zero data (each
entry's bytes 16-19 self-reference an incrementing format index 1-40, useful for correlating against
known ATI/AMD surface format enums later). This is genuinely characterizable, not an opaque mystery -
but decoding every bit-field's real meaning (pitch alignment, tile mode, bytes-per-pixel, etc.) well
enough to use correctly is still real, additional work not completed this pass.

## Honest status

This is real, substantial forward progress on Stage 3's structural blocker: the mechanism for texture/
surface allocation is now identified (an embedded opcode language, not a missing memory type or
selector), several real opcodes are mapped with real semantics, and a source of assumed complexity
(the `&DAT_xxxx` blobs) turned out to be simple literal constants, not opaque data. What remains before
a safe, minimal render-target-and-clear test is possible: fully decoding the per-format lookup table's
bit-field meanings, and correctly sequencing texture allocation -> `build_surface_from_texture` (with
correct, non-depth-buffer parameters) -> fast-clear, each of which still involves real, deeply
state-dependent fields this pass did not fully resolve. The overall picture continues to support
`stage3-scope-assessment.md`'s conclusion: real, tractable, but a materially larger undertaking than
Stages 0-2, now with a much more concrete roadmap than before.
