# Stage 3 (first attempt): native-ISA shader reproduction - real, honest negative result

Attempts to reproduce `void main(){gl_FragColor=vec4(1.0,0.0,0.0,1.0);}` (the exact GLSL shader
already compiled and run successfully in `finish_probe.c`'s `lightProg`, earlier this project) as
hand-encoded native R5xx fragment-ALU (US) instructions, submitted via the exact real PM4 submission
mechanism Stage 2 proved works. **Result: the submission mechanics all succeeded cleanly, but the
hand-encoded instructions had no observable effect on rendering** - a real, informative negative,
not a crash or hang. Two real hypotheses, disambiguation not yet complete; one real side-finding
(correcting an earlier guess) came out of the diagnostic.

## Real instruction encoding, derived directly from AMD's own R5xx Acceleration doc

Full bit-level field tables for `US_CMN_INST`, `US_ALU_RGB_ADDR`, `US_ALU_ALPHA_ADDR`,
`US_ALU_RGB_INST`, `US_ALU_ALPHA_INST`, `US_ALU_RGBA_INST` (§ register reference, MMReg
0xb800/0x9000/0x9800/0xa000/0xa800/0xb000) and the real load mechanism - `GA_US_VECTOR_INDEX`
(MMReg 0x4250) + `GA_US_VECTOR_DATA` (MMReg 0x4254), a 6-dword-per-instruction burst write, since the
US instruction store "cannot be written to directly" per the doc's own text. Also used `US_CODE_ADDR`
(MMReg 0x4630, START_ADDR/END_ADDR) to point the active shader at a chosen instruction slot.

Computed a real, minimal single-instruction OUTPUT program using only swizzle selects (no inline
constants or ALU-constant-register programming needed - a component swizzle can select literal 0,
0.5, or 1 directly): `MAD(A=one/zero-swizzled, B=one, C=zero)` per channel, giving RGB=(1,0,0) or
(0,0,1) depending on which swizzle is chosen for input A, with Alpha=1.0 both times via
`ALPHA_SWIZ_A=one, ALPHA_SWIZ_B=one`. Two real, fully-computed 6-register programs:
- RED (target): `US_CMN_INST=0x001F8105, US_ALU_RGB_ADDR=0, US_ALU_ALPHA_ADDR=0, US_ALU_RGB_INST=0x00DB0498, US_ALU_ALPHA_INST=0x00C18000, US_ALU_RGBA_INST=0x20490000`
- BLUE (control): identical except `US_ALU_RGB_INST=0x00DB0690`

## Real test design (three-point, not a bare visual check)

1. Real, isolated AGL context (own pbuffer, not `hold-context`) - compile+link the real GLSL target
   shader, draw a quad, read back -> **baseline**.
2. Open a second, independent, hand-built IOKit connection (same proven pattern as Stage 1/2), real
   context allocation (Stage 2a's proven sequence).
3. Inject the hand-encoded **BLUE** program at an unused instruction slot (100), redirect
   `US_CODE_ADDR` there, submit via the real Stage 2 mechanism (fill remainder with PM4 Type-2
   filler, trigger re-map, bounded `selector 8` wait). Redraw the **same quad, same already-bound GL
   program object** (no new `glUseProgramObjectARB` call), read back -> **control**. If the redirect
   mechanism works, this should show blue, not the baseline's red - a real control, not just "did it
   look right."
4. Overwrite the same slot with the hand-encoded **RED** program (the real target), submit again,
   redraw, read back -> **test**. Compare against the real baseline.

## Real result

```
baseline (real GLSL):    RGBA=(255,0,0,255)
control  (hand blue):    RGBA=(255,0,0,255)   <- did NOT change to blue
test     (hand red):     RGBA=(255,0,0,255)   <- matches baseline, but inconclusively
```

All submission mechanics succeeded cleanly both times (`re-map: 0x0`, `selector 8 finish: 0x0` on the
first try, matching Stage 2's clean pattern) - no error, no hang, no instability. But the **control
draw never changed color**, meaning the redirect never demonstrably took effect. Since the control
failed to prove the mechanism works, the "test matches baseline" result is **not evidence of a
successful reproduction** - it's equally explained by "my write had no effect at all, so the real
GLSL-compiled shader kept running the whole time." A genuinely honest reading of this data: **the
experiment is inconclusive, not a confirmed success.**

## Real, honest candidate explanations (not yet disambiguated)

1. **Address/protocol mistake**: `GA_US_VECTOR_INDEX`/`GA_US_VECTOR_DATA`/`US_CODE_ADDR` might not be
   reachable via a flat `byte_offset >> 2` Type-0 `BASE_INDEX`, if R5xx separates "config" and
   "context" register address spaces the way some other ATI/AMD GPU generations do (this doc's own
   Type-0 description says BASE_INDEX is literally the DWORD MMIO address, which argues against this,
   but isn't independently confirmed for these specific registers).
2. **AGL re-asserts shader state on every draw**: if Apple's driver unconditionally re-uploads the
   currently-bound program's real compiled microcode as part of its own normal draw-time state
   validation (a common, defensible GPU driver design), my out-of-band write on a separate connection
   would get silently overwritten before the pixel shader ever executes for the verification draw -
   regardless of whether my write itself was completely correct.

Not disambiguated this pass - would need either a live register-readback path (attempted, see below -
not available via the mechanism tried) or a fully self-contained draw issued through my own raw PM4
stream instead of routing back through AGL for the verification draw (a substantially bigger lift:
real vertex format, rasterizer/viewport/render-target register setup, a real draw-primitive packet -
closer to genuine "bare metal" rendering than a shader swap).

## Real side-finding: memory type 0 is NOT a register/status page - it's an embedded plist

Built a read-only diagnostic (`stage3-diag-type0`) hoping memory type 0 (the fixed 4KB region, Stage
0/2-prep's "status/register region" guess) might be a live MMIO passthrough window I could use to
directly verify whether a register write landed. **Real result: it isn't.** Dumping its first 256
bytes shows real, readable ASCII once decoded from hex - genuine XML/plist fragments:
`</integer><key>Q</key>...<key>Frequency</key><data ID="4972">...</data><key>Gain</key>
<data ID="4973">...</data>...<key>Filter3</key>...<key>runInSoftware</key><true/>`. This looks like a
real embedded display/color-calibration profile (gain/frequency/filter entries, keyed IDs) - not a
hardware status page at all. **This corrects Stage 0/2-prep's earlier guess** ("status/register
region" was a reasonable inference from the fixed 4KB size and distinct `options=0x101` mapping flag,
but the real content is something else entirely - likely per-display calibration data the GL driver
reads for color management, unrelated to command submission). Content was byte-identical before and
after the `US_CODE_ADDR` write attempt, confirming this region is not a live register window (ruling
out that specific diagnostic approach, not the underlying register-write question itself).

## Honest status

Stage 3 is **not yet cleanly completed**. Real, substantial groundwork is in place (the exact
instruction encoding, the real load mechanism, a rigorous three-point test design, a working
submission pipeline reused unmodified from Stage 2), and a real, informative negative result was
produced rather than a false positive - but the central hypothesis (native-ISA reproduction of an
already-proven shader) has not been demonstrated. Per the proposal's own logic, Stage 4 (wiring into
the live decode path) is not yet justified by this result. Real next step: either trace the kernel's
real consumption path for `GA_US_VECTOR_INDEX`/`DATA` writes specifically (confirm they're really
reaching the GA block, not silently dropped or misrouted), or build the larger, fully self-contained
raw-PM4 draw needed to test shader execution without depending on AGL's own subsequent draw call.
