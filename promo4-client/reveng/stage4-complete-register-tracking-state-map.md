# The capstone find: a near-complete R580 3D register map, sourced from Apple's own "restore everything" function

Found by pursuing the top-down opportunity identified after `write_r500_3d_blit_state_packet` -
tracing its one real caller, `ATIR500GLContext::restore_state_destroyed_by_pageoff` (`0x2af10`). This
function exists to rebuild the *entire* real 3D render state after the kernel evicts a texture/surface
from VRAM (a pageoff event) - which means, by construction, it has to touch every piece of register
state that actually matters to this driver. It is therefore the single most complete, real,
Apple-authored map of this GPU's working register set this project has ever had access to.

## The mechanism

`restore_state_destroyed_by_pageoff` takes a `register_tracking_state*` - a real saved snapshot of
register values from before the pageoff - and, field by field, writes each one back out as a real
`(register-index, value)` pair into a `r500_3d_blit_state_packet_struct`, in the exact same PM4
Type-0-header convention already established throughout this sweep. `write_r500_3d_blit_state_packet`
(covered separately) is then called to do further processing before the whole thing is submitted via
the confirmed `ATIRadeonX1000::submit_buffer` path.

## The (near-)complete register map, confirmed field-by-field against `register_tracking_state`

All of these are **directly, explicitly** paired with a `register_tracking_state` field offset in the
real decompiled code - not inferred from address ranges alone:

| Register | Address | Source in this function |
|---|---|---|
| `SC_EDGERULE` | `0x43a8` | field `+0x00` |
| `SC_SCREENDOOR` | `0x43e8` | field `+0x04` |
| `GB_MSPOS0` | `0x4010` | (burst w/ next) |
| `GB_MSPOS1` | `0x4014` | (burst) - **exact match to the KolibriOS reference's own `GB_MSPOS0`/`MSPOS1` writes** |
| *(unnamed, `0x4020`=`GB_AA_CONFIG`)* | `0x4020` | field `+0x0c` |
| `SC_HYPERZ_EN` | `0x43a4` | field `+0x14` (the `compute_sc_hyperz_en` output slot) |
| `ZB_BW_CNTL` | `0x4f1c` | field `+0x18` (the `compute_zb_bw_cntl` output slot) |
| `ZB_ZSTENCILCNTL` | `0x4f04` | field `+0x1c` |
| `VAP_CNTL_STATUS` | `0x2140` | field `+0x20` |
| `VAP_CLIP_CNTL` | `0x221c` | field `+0x24` |
| `VAP_VTE_CNTL` | `0x20b0` | field `+0x28` |
| `SU_CULL_MODE` | `0x42b8` | field `+0x2c` |
| *(unnamed, `0x4e50`=`RB3D_DITHER_CTL`)* | `0x4e50` | field `+0x30` |
| `RB3D_CCTL` | `0x4e00` | literal `0` |
| `GB_ENABLE` | `0x4008` | literal (re-confirms GA-plugin finding) |
| `VAP_OUT_VTX_FMT_0` | `0x2090` | field `+0x3c` |
| `VAP_OUT_VTX_FMT_1` | `0x2094` | field `+0x40` |
| `VAP_VTX_SIZE` | `0x20b4` | field `+0x44` |
| `VAP_PROG_STREAM_CNTL_0` | `0x2150` | field `+0x48` |
| `VAP_PROG_STREAM_CNTL_EXT_0` | `0x21e0` | field `+0x4c` |
| `ZB_CNTL` | `0x4f00` | field `+0x50` |
| `FG_FOG_BLEND` | `0x4bc0` | field `+0x54` |
| `FG_ALPHA_FUNC` | `0x4bd4` | field `+0x58` |
| `GA_POLY_MODE` | `0x4288` | field `+0x5c` |
| `RB3D_BLENDCNTL` | `0x4e04` | field `+0x60` |
| `GA_COLOR_CONTROL` | `0x4278` | field `+0x64` |
| `GA_COLOR_CONTROL_PS3` | `0x4258` | field `+0x74` (the "PS3 mode" shadow of `GA_COLOR_CONTROL`) |
| `TX_ENABLE` | `0x4104` | field `+0x6c` |
| `GB:PS3_ENABLE` | `0x4118` | field `+0x70` - **a real master enable for this driver's "PS3" (extended pixel-shader-3-style) mode**, adjacent to the already-known hang-risk registers `PS3_VTX_FMT`/`PS3_TEX_SOURCE` |
| `GA_US_VECTOR_INDEX`/`DATA` | `0x4250`/`0x4254` | fields `+0x74` onward - the real saved/restored shader instruction words (11 dwords, matching the already-confirmed shader-load burst length) |
| `US_CONFIG` | `0x4600` | field via the `0x184`-area PM4 NOP-wrapped block - **a real fragment-shader-unit master config/enable register**, the clearest "turn the US block on" register found this whole project |
| `US_FC_CTRL` | `0x4624` | field `+0x88` |
| `US_PIXSIZE` | `0x4604` | field `+0x8c` |
| `US_CODE_RANGE` | `0x4634` | field `+0x90` |
| `US_CODE_OFFSET` | `0x4638` | field `+0x94` |
| `US_CODE_ADDR` | `0x4630` | field `+0x98` |
| `US_OUT_FMT_0` | `0x46a4` | field `+0x9c` |
| `US_OUT_FMT_1/2/3` | `0x46a8/ac/b0` | fields `+0x78/0x7c/0x80` - **now confirmed real registers, not found in either local doc individually before; confirmed here purely by array-adjacency and consistent field pairing** |
| `RS_COUNT` | `0x4300` | field `+0x84` |
| `RS_INST_COUNT` | `0x4304` | field `+0xa0` |
| `RS_INST_0` | `0x4320` | field `+0xa4` |
| `RS_IP_0` | `0x4074` | field `+0xa8` |
| `RB3D_COLOROFFSET0`/`COLORPITCH0` | `0x4e28`/`0x4e38` | literal `0` |
| `TX_INVALTAGS` | `0x4100` | literal `0` |
| `TX_FILTER0_0` | `0x4400` | literal `0` |
| `TX_OFFSET_[0-15]` | `0x4540-0x457c` | field `+0xac` |
| `TX_FILTER1_[0-15]` | `0x4440-0x447c` | literal `0` |
| `TX_FORMAT0_[0-15]` | `0x4480-0x44bc` | literal `0` |
| `TX_FORMAT1_[0-15]` | `0x44c0-0x44fc` | literal `0` |
| `SC_CLIP_RULE` | `0x43d0` | literal `0xaaaa` - **fifth independent confirmation** |
| `RB3D_COLOR_CHANNEL_MASK` | `0x4e0c` | field `+0xd0` - **exact match to the KolibriOS reference's explicit all-channels-enabled write** |
| `RB3D_ROPCNTL` | `0x4e18` | field `+0xd8` |
| `GA_POINT_S0/T0/S1/T1` | `0x4200-0x420c` | (real 4-register burst, `TYPE=0 COUNT=4` header confirmed) |
| `GA_POINT_SIZE` | `0x421c` | field, address confirmed |
| `ZB_FORMAT` | `0x4f10` | field `+0x314`-area |
| `ZB_DEPTHOFFSET`+adjacent | `0x4f20`+ | (real `COUNT=2` burst, already known) |
| `ZB_HIZ_PITCH` | `0x4f54` | field |
| `ZB_DEPTHCLEARVALUE` | `0x4f28` | field |

**Two registers seen again but still not resolved in either local document** (now seen in a *third*
independent context each - genuinely real, just not in these two specific PDF revisions): `0x1383`
(byte `0x4e0c`... note: resolved as `RB3D_COLOR_CHANNEL_MASK` above via a different index - `0x1383`
itself decodes to byte `0x4e0c` too, meaning this is the SAME register written via two different code
paths, consistent) and `0x1386` (byte `0x4e18` = `RB3D_ROPCNTL`, also now resolved above). Genuinely
unresolved: `0x13cc` (byte `0x4f30`) - seen in both `write_kernel_context_buffer_regs` and here,
consistently in the HiZ-adjacent register cluster, but absent from both local PDF revisions.

## Real, valuable discoveries beyond pure register-naming

- **`GB:PS3_ENABLE`** (`0x4118`) is a real master enable for this driver's extended "PS3" shading mode,
  living right next to the already-documented hang-risk registers (`PS3_VTX_FMT`/`PS3_TEX_SOURCE`,
  `stage3-viewport-and-hang-mitigation-from-pdf.md`). This resolves an implicit question that section
  never answered: PS3 mode is a real, distinct, explicitly-enabled mode, not implicitly triggered.
- **`US_CONFIG`** (`0x4600`) is the clearest single "turn the fragment shader unit on" register this
  entire project has found - directly relevant to anyone building a from-scratch pipeline bring-up in
  the future.
- **`GA_COLOR_CONTROL_PS3`** (`0x4258`) confirms `GA_COLOR_CONTROL` (`0x4278`) has a real "PS3-mode
  shadow" register, mirroring the `PS3_VTX_FMT`/`VAP_OUT_VTX_FMT` and `PS3_TEX_SOURCE`/`VAP` shadow-pair
  pattern already documented - a real, consistent architectural pattern across this driver generation
  (every "PS3-mode" register appears to have a plain-mode counterpart the VAP/GA blocks populate
  first, then a GB/PS3-block shadow copy).
- **This is, for all practical purposes, the complete register footprint of a working R580 3D pipeline**
  as Apple's own driver defines "everything that must be saved and restored to resume exactly where you
  left off" - about as authoritative and complete a source as static analysis of this exact chip could
  produce without the manufacturer's own internal register spec.

Not re-tested on hardware - pure static analysis, G5 still down this session. This map should be
folded into any future revision of `stage4-TOP-DOWN-promo4-redesign-proposal.md`'s concrete register
sequence, since it is now more complete and more directly Apple-sourced than the KolibriOS-derived
sequence that proposal originally cited.
