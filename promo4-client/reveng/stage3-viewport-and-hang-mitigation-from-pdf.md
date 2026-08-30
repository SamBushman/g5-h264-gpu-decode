# Real viewport/vertex-format registers, and a concrete mitigation for the documented hang risk

Pure documentation research (no hardware touched), done at the user's request while the G5 remains
down for an extended physical power-cycle. `stage3-scope-assessment.md` flagged viewport and vertex-
format register setup as genuinely undocumented gaps blocking any future "fully self-contained draw"
attempt (Stage 3's biggest remaining undertaking). Mined two docs from the local AMD collection
(`~/Documents/AMD Docs/`) not previously used by this project: `R3xx_3D_Registers.pdf` (a dedicated
register reference for the closely-related R3xx family, which shares most of the R5xx 3D pipeline) and
a fresh pass over `R5xx_Acceleration_v1.5.pdf` (already used for other sections, but not this one).
Extracted via `ps2ascii` per the established method (`reference-amd-gpu-docs`).

## Real viewport transform registers (from `R3xx_3D_Registers.pdf`) - fills a genuine, previously-flagged gap

| Register | MMReg | Purpose |
|---|---|---|
| `VAP_VPORT_XOFFSET` | `0x1d9c` / `0x209c` | Viewport X offset (IEEE float) |
| `VAP_VPORT_XSCALE` | `0x1d98` / `0x2098` | Viewport X scale (IEEE float) |
| `VAP_VPORT_YOFFSET` | `0x1da4` / `0x20a4` | Viewport Y offset (IEEE float) |
| `VAP_VPORT_YSCALE` | `0x1da0` / `0x20a0` | Viewport Y scale (IEEE float) |
| `VAP_VPORT_ZOFFSET` | `0x1dac` / `0x20ac` | Viewport Z offset (IEEE float) |
| `VAP_VPORT_ZSCALE` | `0x1da8` / `0x20a8` | Viewport Z scale (IEEE float) |
| `VAP_VTE_CNTL` | `0x20b0` | Enable bits for each scale/offset above, plus `VTX_XY_FMT`/`VTX_Z_FMT`/`VTX_W0_FMT` (whether incoming vertex data is pre-divided by W) and `SERIAL_PROC_ENA` |

Each register has two listed `MMReg` addresses (e.g. `0x1d9c` and `0x209c`) - two different aliases for
the same logical register, consistent with this family's known pattern of shadow/mirrored register
ranges (not yet cross-checked against which alias this project's already-confirmed working register
writes use elsewhere, e.g. `GA_US_VECTOR_INDEX`/`US_CODE_ADDR` in the existing shader-injection work).

Real, also-relevant scissor registers found alongside: `SC_SCISSOR0`/`SC_SCISSOR1` (`0x43e0`/`0x43e4`),
packing left/upper and right/lower scissor-rectangle edges into bits `12:0`/`25:13` of each dword.

## Real output vertex-format registers (`VAP_OUT_VTX_FMT_0/1`, `R3xx_3D_Registers.pdf`)

`VAP_OUT_VTX_FMT_0` (MMReg `0x2090`): single-bit presence flags - `VTX_POS_PRESENT` (bit 0),
`VTX_COLOR_0..3_PRESENT` (bits 1-4), `VTX_PT_SIZE_PRESENT` (bit 16).
`VAP_OUT_VTX_FMT_1` (MMReg `0x2094`): per-texcoord component counts, 3 bits each (`TEX_0..9_COMP_CNT`,
0=not present, 1-4=that many components), packed across the full 32-bit register (bits `2:0`, `5:3`,
`8:6`, ... up through `TEX9`).

## The documented hang risk (`PS3_VTX_FMT`/`PS3_TEX_SOURCE`, §10.9.5) - WITH its real, concrete fix

`stage3-scope-assessment.md` cited this section's existence as evidence of real hang risk in adjacent
register territory, but hadn't quoted the actual mitigating guidance. The full text, verbatim:

> Writes to the PS3_VTX_FMT and PS3_TEX_SOURCE register can cause bad textures or hangs in R5xx chips,
> if followed immediately by VF_CNTL writes (i.e. draw command). **Following any of these 2 registers
> with 2 register writes (to GA or any block below) will always avoid the problem, before the next
> VF_CNTL.**

This is a real, specific, actionable mitigation, not just a warning to stay away - a future
"fully self-contained draw" attempt CAN safely touch these registers, as long as it inserts two more
register writes (to any GA-block-or-later register - real candidates already known from this project's
own findings: `GA_US_VECTOR_INDEX`, or any of the `GA_*` registers freshly catalogued below) between
setting vertex format and issuing the actual draw command (`VAP_VF_CNTL`, MMReg `0x2084` - the same
register this project's own `stage3-scope-assessment.md` already identified as governing
`3D_DRAW_IMMD_2`).

`PS3_TEX_SOURCE` (`GB` block, MMReg `0x4120`) and `PS3_VTX_FMT` (`GB` block, MMReg `0x411c`) are both a
**downstream ("GB" - a later setup/rasterizer-adjacent stage) shadow copy** of vertex-format
information the `VAP` block already computes via `VAP_OUT_VTX_FMT_0/1` above - `PS3_TEX_SOURCE`
additionally specifies, per texture unit, whether to replicate the VAP's own source coordinates or
"stuff" fixed `GA`-generated `(S,T)`/`(S,T,R)` values instead. The hang risk is very plausibly a real
pipeline hazard in keeping these two shadow copies (VAP's own format register vs. GB's mirrored one)
synchronized correctly if a draw is triggered too soon after updating the mirror - the documented
2-register-write buffer is presumably enough real pipeline latency for that hazard window to clear.

## Real `GA`-block register catalog found along the way (not individually explored, but real MMReg addresses now on record)

`GA_COLOR_CONTROL` (`0x4278`), `GA_ENHANCE` (`0x4274`), `GA_FOG_OFFSET`/`GA_FOG_SCALE` (`0x4298`/
`0x4294`), `GA_LINE_CNTL`/`GA_LINE_S0`/`GA_LINE_S1`/`GA_LINE_STIPPLE_CONFIG`/`GA_LINE_STIPPLE_VALUE`
(`0x4234`/`0x4264`/`0x4268`/`0x4238`/`0x4260`), `GA_OFFSET` (`0x4290`), `GA_POINT_MINMAX`/`GA_POINT_S0`/
`GA_POINT_S1`/`GA_POINT_SIZE`/`GA_POINT_T0`/`GA_POINT_T1` (`0x4230`/`0x4200`/`0x4208`/`0x421c`/`0x4204`/
`0x420c`), `GA_POLY_MODE` (`0x4288`), `GA_ROUND_MODE` (`0x428c`), `GA_SOFT_RESET` (`0x429c`),
`GA_SOLID_BA` (`0x4280`). Any of these (being genuinely harmless, low-risk state registers) are
reasonable, safe candidates for the "2 dummy register writes" the hang-mitigation above calls for.

## Honest gap not resolved this pass

Searched for an explicit rasterizer/scan-converter "master enable" register (`stage3-scope-
assessment.md`'s other flagged gap, alongside viewport/vertex-format) under several likely naming
patterns (`RS_CNTL`, `*_ENABLE`, `CULL_ENABLE`) in `R3xx_3D_Registers.pdf` - no direct hit. Either it's
named differently than guessed, implicitly controlled by whether any vertex-format bits are set, or
governed by a register block not yet searched. Worth a more targeted pass (e.g. reading the actual
Rasterizer Registers section, §1.5 per this doc's own table of contents, sequentially rather than by
keyword guess) if this project resumes the "fully self-contained draw" effort.
