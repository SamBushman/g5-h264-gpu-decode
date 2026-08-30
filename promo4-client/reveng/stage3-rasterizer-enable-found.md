# The "rasterizer enable" gap resolved: two registers that gate ALL visibility, both default to "nothing renders"

Follow-up to `stage3-viewport-and-hang-mitigation-from-pdf.md`, which searched `R3xx_3D_Registers.pdf`
for an explicit rasterizer/scan-converter "master enable" under several guessed names and came up
empty. Found it under names that weren't guessed, and cross-validated against a real, working,
open-source driver's actual code - not just AMD's register descriptions in isolation.

## The two real registers, both in the `SC` (Scissor/Clip) block

**`SC_SCREENDOOR`** (MMReg `0x43e8`): a 24-bit "screen door sample mask" - `1` means a sample may be
covered (i.e. rendered), `0` means it cannot. **Defaults to `0x0` - every sample blocked, by design,
until a client explicitly sets it.**

**`SC_CLIP_RULE`** (MMReg `0x43d0`): a 16-bit truth table, indexed by a 4-bit "which of the 4 clip
rectangles is this pixel inside" pattern, giving the final inside/outside visibility decision.
**Defaults to `0x0`** - meaning even the "inside no clip rect" case (index 0, the natural default state
before any clip rectangle is configured) resolves to "not visible."

Together, these mean a real client MUST explicitly write both registers with real, correct values
before anything can ever become visible - not a "some cases might not render," a hard, universal
precondition. This is very likely exactly the gap `stage3-scope-assessment.md` was gesturing at with
"rasterizer/scan-converter enable... still undocumented in this project's own findings," even though
neither register name matches an "enable" naming pattern directly.

## Cross-validated against real, working driver code - not just AMD's docs in isolation

Found via `gh search code "SC_CLIP_RULE"`: `KolibriOS/kolibrios:drivers/old/ati2d/init_3d.inc` (real,
working ATI/VA-Linux-derived R300-R500 3D initialization code, same lineage as the X11Libre
`xf86-video-ati` fork already used elsewhere in this project) - **and it explicitly branches on
`CHIP_FAMILY_R580`**, this project's own exact target chip, with chip-specific tuning
(`R300_PVS_NUM_FPUS_SHIFT` gets `8` specifically for `R520`/`R580`, vs. `4`-`6` for other chips). About
as directly relevant a real-world reference as this project could hope to find.

The real values this working code writes:
```c
OUT_ACCEL_REG(R300_SC_EDGERULE, 0xA5294A5);
// R500-family branch (R580 included):
OUT_ACCEL_REG(R300_SC_CLIP_0_A, (0 << R300_CLIP_X_SHIFT) | (0 << R300_CLIP_Y_SHIFT));
OUT_ACCEL_REG(R300_SC_CLIP_0_B, (4080 << R300_CLIP_X_SHIFT) | (4080 << R300_CLIP_Y_SHIFT));
OUT_ACCEL_REG(R300_SC_CLIP_RULE, 0xAAAA);
OUT_ACCEL_REG(R300_SC_SCREENDOOR, 0xffffff);
```
- `SC_CLIP_0_A`/`SC_CLIP_0_B` sized to a large fixed bound (0,0)-(4080,4080) - effectively "cover the
  whole practical screen," rather than anything tied to the actual render target size, at least at
  this stage of setup (real per-draw scissoring presumably still uses the separate `SC_SCISSOR0/1`
  registers `stage3-viewport-and-hang-mitigation-from-pdf.md` already catalogued).
- `SC_CLIP_RULE = 0xAAAA` (binary `1010101010101010`): every ODD-indexed table entry is `1`, every
  EVEN-indexed entry is `0` - since bit 0 of the 4-bit index corresponds to "inside clip rect 0," this
  is exactly the truth table for "visible iff inside clip rect 0" (ignoring rects 1-3 entirely) - the
  simplest possible "use just one bounding rectangle" configuration.
- `SC_SCREENDOOR = 0xffffff` - all 24 sample-mask bits set, i.e. "every sample may be covered."
- `SC_EDGERULE = 0xA5294A5` - the real, validated production value for the edge-rule register
  `stage3-viewport-and-hang-mitigation-from-pdf.md` already catalogued the bit-field meaning of but
  didn't have a real working value for.

## Bonus: real, validated `VAP_CNTL`/`VAP_VTE_CNTL` values, R580-specific

The same file's VAP (vertex assembly) setup, also directly relevant to a future self-contained draw:
```c
vap_cntl = (10 << R300_PVS_NUM_SLOTS_SHIFT) | (5 << R300_PVS_NUM_CNTLRS_SHIFT) |
           (5 << R300_VF_MAX_VTX_NUM_SHIFT);
// R520/R580-specific:
vap_cntl |= (8 << R300_PVS_NUM_FPUS_SHIFT);
OUT_ACCEL_REG(R300_VAP_CNTL, vap_cntl);
OUT_ACCEL_REG(R300_VAP_VTE_CNTL, R300_VTX_XY_FMT | R300_VTX_Z_FMT);
```
`VAP_VTE_CNTL`'s real value confirms and extends `stage3-viewport-and-hang-mitigation-from-pdf.md`'s
viewport register catalog: real working code sets `VTX_XY_FMT` and `VTX_Z_FMT` (incoming X/Y/Z already
divided by W - i.e. don't ask the Setup Engine to do the perspective divide), but leaves `VTX_W0_FMT`
unset (incoming W0 genuinely is `1/W0` already, not needing a reciprocal) - a real, concrete, working
answer to a question that PDF register description alone left ambiguous (which of the three FMT bits a
real client actually sets).

## Honest scope note

This is real, validated, directly relevant reference material for the "fully self-contained draw"
effort `stage3-scope-assessment.md` characterized as a materially larger undertaking than anything else
in this thread - it substantially de-risks that future effort's viewport/vertex-format/visibility setup
specifically, but doesn't change that assessment's overall conclusion (real render-target/surface
allocation via the embedded opcode language, and the full HyperZ/texture-offset machinery
`write_r500_3d_blit_state_packet` uses, remain separate, unresolved pieces of that larger scope). Not
re-tested on hardware - pure documentation/reference-code research, G5 still down.
