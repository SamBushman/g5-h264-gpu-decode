# A complete, real, working reference for render-target setup and a full draw - the last major scope-assessment gap

Follow-up to `stage3-rasterizer-enable-found.md`, continuing at the user's direction to dig into
render-target/surface allocation - the other big piece `stage3-scope-assessment.md` flagged as missing
for a "fully self-contained draw." Found a complete, real, working reference by reading further into
the same `KolibriOS/kolibrios:drivers/old/ati2d/accel_3d.inc` file (`R300PrepareComposite`/
`RadeonCompositeTile`/`R300TextureSetup`) already used for the rasterizer-enable finding. This function
implements exactly the kind of operation this project has been trying to reach: a real GPU-composited
textured quad draw, register-level, no software fallback.

## Real color-buffer (render-target) setup - the missing piece, concretely

```c
dst_offset = rhd.FbIntAddress + rhd.FbScanoutStart;   // framebuffer base + scanout offset
dst_pitch  = rhd.displayWidth * 4;                    // bytes/line for ARGB8888
colorpitch = (dst_pitch >> pixel_shift) | dst_format;  // pixel_shift = 32>>4 = 2 for this format
OUT_ACCEL_REG(R300_RB3D_COLOROFFSET0, dst_offset);
OUT_ACCEL_REG(R300_RB3D_COLORPITCH0, colorpitch);
```
Real, simple, working formula - `RB3D_COLOROFFSET0`/`RB3D_COLORPITCH0` just need a raw byte offset and
a pitch-plus-format-tag dword. Real alignment requirements enforced by this code's own sanity checks:
offset must be 16-byte aligned (`& 0x0f`), pitch's shifted value must be a multiple of 8 (`& 0x7`).

## HyperZ/Z-buffer: genuinely NOT needed for a minimal test - a real scope reduction, not just a gap fill

Checked the real register defaults in `R3xx_3D_Registers.pdf`: **`SC_HYPERZ_EN.HZ_EN` defaults to
`0x0`** (disabled) and **`ZB_CNTL.Z_ENABLE`/`STENCIL_ENABLE` both default to `0x0`** (disabled). This
directly reduces `stage3-scope-assessment.md`'s stated scope: that assessment flagged
`compute_sc_hyperz_en`/`compute_zb_bw_cntl` (real HyperZ/Z-bandwidth subsystem calls inside Apple's own
`write_r500_3d_blit_state_packet`) as "an entire real subsystem... not yet studied in depth" - but
Apple's driver calls those because it handles the fully general case (which MIGHT have depth testing
on). **A minimal, purpose-built test client does not need to replicate that generality** - HyperZ and
the Z/stencil pipeline are off by default, so a client that never explicitly enables them can skip that
whole subsystem entirely, real confirmed default values, not an assumption.

## Real, validated vertex-input and draw-command sequence, cross-validating this project's own shader-encoding work

`VAP_PROG_STREAM_CNTL_0` (vertex input format - 2 attributes, each a `FLOAT_2`: position, then
texcoord, `DST_VEC_LOC` 0 and 6) and `VAP_PVS_CODE_CNTL_0/1` (selects a pre-loaded vertex program by
instruction-slot range) are both real, working, and now on record with concrete encodings this project
hadn't captured before.

The real draw trigger:
```c
OUT_RING(CP_PACKET3(R200_CP_PACKET3_3D_DRAW_IMMD_2, 4 * vtx_count));
OUT_RING(RADEON_CP_VC_CNTL_PRIM_TYPE_TRI_FAN | RADEON_CP_VC_CNTL_PRIM_WALK_RING | (4 << NUM_SHIFT));
VTX_OUT(x, y, u, v);   // x4, one per vertex - raw floats, immediate-mode
OUT_ACCEL_REG(R300_RB3D_DSTCACHE_CTLSTAT, R300_DC_FLUSH_3D);   // real post-draw cache flush
```
**`3D_DRAW_IMMD_2` is the exact same PM4 opcode `stage3-scope-assessment.md` already identified from
the AMD spec alone** - real, independent cross-validation from working driver code, not just a spec
reading. The header dword's real, working value (`PRIM_TYPE_TRI_FAN | PRIM_WALK_RING | (4<<NUM_SHIFT)`)
was previously undocumented in this project. The trailing `RB3D_DSTCACHE_CTLSTAT` flush matches this
project's own already-established convention (flush the destination cache before trusting a
readback) - real, independent confirmation of a pattern already in use.

## Real fragment-shader construction via `GA_US_VECTOR_DATA` - direct cross-validation of `stage3_native_shader.c`

The same file constructs a real, complete R500 fragment-shader program (a texture-modulate-and-blend
shader for compositing) using `R500_GA_US_VECTOR_DATA` writes - **the exact same register this
project's own `stage3_native_shader.c` already uses** for its hand-encoded shader load. Seeing a real,
working, independently-authored shader constructed via the identical instruction-field macros
(`R500_INST_TYPE_OUT`, `R500_ALU_RGB_SEL_A_SRC0`, `R500_ALU_RGBA_OP_MAD`, etc.) is strong, independent
corroboration that this project's own derived encoding for `US_CMN_INST`/`US_ALU_RGB_INST`/etc. is
using the real, correct bit-field layout, not just AMD's spec read in isolation.

## Real per-texture-unit setup (`R300TextureSetup`) - the raw-register surface/texture path

For any future work that wants to bypass Apple's embedded-opcode-language texture path
(`stage3-embedded-opcode-language.md`) entirely and talk to texture units directly:
```c
OUT_ACCEL_REG(R300_TX_FILTER0_0 + unit*4, txfilter);      // wrap/clamp + min/mag filter, per unit
OUT_ACCEL_REG(R300_TX_FILTER1_0 + unit*4, 0);
OUT_ACCEL_REG(R300_TX_FORMAT0_0 + unit*4, txformat0);     // (width-1)<<shift | (height-1)<<shift | TXPITCH_EN
OUT_ACCEL_REG(R300_TX_FORMAT1_0 + unit*4, txformat1);     // real pixel-format tag (R300_EASY_TX_FORMAT macro)
OUT_ACCEL_REG(R300_TX_FORMAT2_0 + unit*4, txpitch);       // (pitch_in_texels >> shift) - 1
OUT_ACCEL_REG(R300_TX_OFFSET_0 + unit*4, txoffset);       // raw byte offset, 32-byte aligned
OUT_ACCEL_REG(R300_TX_BORDER_COLOR_0 + unit*4, 0);
```
Real, working, per-unit register layout (registers indexed by `unit*4`) - genuinely useful groundwork
if a future attempt ever needs to bind a texture without going through the embedded-opcode language.

## Overall assessment: this substantially de-risks the "fully self-contained draw" effort

Combined with `stage3-rasterizer-enable-found.md`'s viewport/visibility findings, this project now has
a complete, real, cross-validated, end-to-end register-level recipe for a minimal textured draw on this
exact chip family (color-buffer setup, vertex format, shader construction via the already-proven
`GA_US_VECTOR_DATA` mechanism, the real draw packet, and a real post-draw sync convention) - PLUS a
genuine scope reduction (HyperZ/Z-buffer can be skipped entirely for a minimal test, not just
"eventually needs solving"). `stage3-scope-assessment.md`'s "materially larger undertaking" framing was
correct at the time it was written, but a meaningful fraction of what made it larger (undocumented
viewport/vertex-format registers, unclear render-target setup, assumed-necessary HyperZ complexity) is
now resolved or shown unnecessary. Not re-tested on hardware - pure documentation/reference-code
research, G5 still down. The remaining genuinely open piece is the embedded-opcode-language route
specifically (if a future attempt wants to stay inside Apple's existing command-buffer conventions
rather than going fully bare-metal via raw PM4 as this reference material demonstrates) - a choice
between two now-better-understood paths, not an unknown gap either way.
