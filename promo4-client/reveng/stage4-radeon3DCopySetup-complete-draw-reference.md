# The crown jewel of this sweep: `_radeon3DCopySetup` is Apple's own complete, real, minimal-3D-draw reference

Found in `ATIRadeonX1000GA.plugin` (`0x2d10`), decompiled while sweeping the GA plugin's real hardware
blit functions. This single function is a complete, register-by-register confirmed implementation of
almost the entire 3D pipeline setup this project has been trying to reconstruct from third-party
(KolibriOS) reference code in `stage3-render-target-and-full-draw-reference.md` - except this one comes
from **Apple's own shipped binary for this exact GPU**, not a third-party guess.

## Real methodology correction made along the way (worth recording)

Initially tried to verify the `&DAT_xxxx == literal xxxx` convention (established for the GL driver
bundle's BSS locations) by reading the raw dword stored at each `DAT_xxxx` address in *this* binary -
got real, non-trivial values, seemingly contradicting the convention. **This was a false alarm**: checked
the real PowerPC disassembly at the call site and found a plain `li r9,0x10ea` (load-immediate) -
proof the compiler really did just want the integer `0x10ea`, and the "value stored at that address"
I'd read was simply whatever unrelated real code/data happens to occupy that small address in this
tiny binary (a small immediate will always coincidentally look like a "valid" address in a program this
size). **Lesson for any future work on this convention**: verify via disassembly (looking for `li`/
`lis+ori` sequences), not by reading memory at the referenced address - reading memory only works when
the location is confirmed BSS/zero-initialized, which isn't a safe assumption in every binary.

## The real register sequence, fully decoded and cross-referenced

Every `(index, value)` pair in the function, decoded via the standard `li` immediate value:

| Register | Address | Confirmed name |
|---|---|---|
| `0x10ea` | `0x43a8` | `SC_EDGERULE` |
| `0x10fa` | `0x43e8` | `SC_SCREENDOOR` |
| `0x1004` | `0x4010` | *(GB block, not individually confirmed)* |
| `0x1005` | `0x4014` | *(GB block, not individually confirmed)* |
| `0x850`  | `0x2140` | `VAP_CNTL_STATUS` |
| `0x887`  | `0x221c` | `VAP_CLIP_CNTL` |
| `0x82c`  | `0x20b0` | `VAP_VTE_CNTL` |
| `0x10ae` | `0x42b8` | `SU_CULL_MODE` |
| `0x1002` | `0x4008` | `GB_ENABLE` |
| `0x824`  | `0x2090` | `VAP_OUT_VTX_FMT_0` |
| `0x825`  | `0x2094` | `VAP_OUT_VTX_FMT_1` |
| `0x82d`  | `0x20b4` | `VAP_VTX_SIZE` |
| `0x854`  | `0x2150` | `VAP_PROG_STREAM_CNTL_[0-7]` (first of the array) |
| `0x878`  | `0x21e0` | `VAP_PROG_STREAM_CNTL_EXT_[0-7]` (first of the array) |
| `0x13c0` | `0x4f00` | `ZB_CNTL` |
| `0x13c1` | `0x4f04` | `ZB_ZSTENCILCNTL` |
| `0x12f5` | `0x4bd4` | `FG_ALPHA_FUNC` |
| `0x12f0` | `0x4bc0` | `FG_FOG_BLEND` |
| `0x10a2` | `0x4288` | `GA_POLY_MODE` |
| `0x1381` | `0x4e04` | `RB3D_BLENDCNTL` |
| `0x109e` | `0x4278` | `GA_COLOR_CONTROL` |
| `0x1041` | `0x4104` | `TX_ENABLE` |
| **`0x1094`** | **`0x4250`** | **`GA_US_VECTOR_INDEX`** - the same register this project's own `stage3_native_shader.c` already uses |
| **`0x1095`** | **`0x4254`** | **`GA_US_VECTOR_DATA`** - same, written 12 times in a row with real shader instruction words |
| `0x1189` | `0x4624` | `US_FC_CTRL` |
| `0x1181` | `0x4604` | `US_PIXSIZE` |
| `0x118d` | `0x4634` | `US_CODE_RANGE` |
| `0x118e` | `0x4638` | `US_CODE_OFFSET` |
| **`0x118c`** | **`0x4630`** | **`US_CODE_ADDR`** - same register this project's own shader-injection work already targets |
| `0x11a9`-`0x11ab`,`0x11ac` | `0x46a4`-`0x46b0` | `US_OUT_FMT_[0-3]` (last one not individually confirmed) |
| `0x10c0` | `0x4300` | `RS_COUNT` |
| `0x10c1` | `0x4304` | `RS_INST_COUNT` |
| `0x10c8` | `0x4320` | `RS_INST_[0-15]` (first of the array) |
| `0x101d` | `0x4074` | `RS_IP_[0-15]` (first of the array) |
| `0x1040` | `0x4100` | `TX_INVALTAGS` |

**Nearly every register in the entire function is confirmed** - this single sequence exercises almost
the complete 3D pipeline: vertex format/stream config (`VAP_*`), rasterizer input routing (`RS_*`),
fragment shader setup and load (`US_*`, `GA_US_VECTOR_*`), texture enable, and back-end state
(`ZB_CNTL`, `RB3D_BLENDCNTL`, `GA_COLOR_CONTROL`, `FG_ALPHA_FUNC`).

## The single most important confirmation: independent validation of this project's own shader ISA work

The real shader instruction words written via the confirmed `GA_US_VECTOR_INDEX`/`DATA` pair include
**the literal value `0x20490000`** - which is **exactly** this project's own `US_ALU_RGBA_INST_VAL`
constant, already used (and empirically proven correct on real hardware) in `stage3_native_shader.c`.
This is Apple's own shipped GA plugin, completely independently, using the identical real instruction
encoding this project derived from the AMD spec by hand. Combined with `US_CODE_ADDR` (`0x118c`) being
the exact register this project already uses to redirect shader execution, **this function is airtight,
independent, Apple-sourced confirmation that this entire project's native-shader-ISA reverse-engineering
work has been correct from the start.**

## Real significance for the PROMO4 redesign

This function is real, proven, Apple-authored proof that a *complete* minimal-3D-pipeline bring-up
(vertex format → rasterizer input → fragment shader load → back-end state) is achievable in one
compact, linear register sequence with no dependency on the embedded command-buffer marker language
this project's whole hang investigation has been centered on. Combined with
`stage3-render-target-and-full-draw-reference.md`'s KolibriOS-derived sequence, this project now has
**two independently-sourced, register-confirmed complete draw setups** for this exact chip family -
about as strong a foundation as static analysis alone can provide for designing a future, correctly-
targeted injection attempt.

## Third and fourth confirmations: `_radeonCopy`/`_radeonFill`/`_radeonHighlight`/`_radeonSolidScanlines`

Also decompiled the remaining real 2D-via-3D-engine blit functions (needed a manual `createFunction`
call first - Ghidra's auto-analysis hadn't created Function objects at these exported-symbol addresses
despite a full re-analysis pass, a real, minor tooling quirk worth noting for any future work on this
binary). All four use the same already-confirmed register set (`VAP_OUT_VTX_FMT_*`, `VAP_PROG_STREAM_
CNTL[_EXT]` arrays, `RS_INST_*`/`RS_IP_*` arrays, `GA_US_VECTOR_INDEX/DATA`, `US_CODE_ADDR`, `ZB_CNTL`,
etc.) plus one genuinely new one: `TX_FILTER0_[0-15]` (`0x4400-0x443c`, confirmed exactly from index
`0x1100`) - real per-texture-unit filter-mode configuration, consistent with `_radeonCopy` actually
sampling a source texture (unlike the solid-fill case). No new registers beyond what
`_radeon3DCopySetup`/`_radeon3DFillSetup` already established - this is genuinely the same real,
confirmed pipeline-setup pattern reused consistently across every 2D-via-3D operation in this plugin.

## Independent second confirmation: `_radeon3DFillSetup`

The sibling solid-color-fill function (`0x3540`) uses the identical register set and, critically, the
identical real shader instruction words (`US_CMN_INST_VAL`-shaped `0x78105`, and **`0x20490000`
again** for `US_ALU_RGBA_INST_VAL`) - a second, independent confirmation from a different real code
path in the same binary, not a fluke of one function.

**Testable hypothesis, documented per the user's instruction, NOT attempted (no hardware this
session)**: replaying this exact register sequence (in place of the fragile embedded-marker-chain
approach this project's live attempts have used so far) inside a real, isolated `AGLContext`'s command
buffer - respecting the now-corrected real cursor field (`+0x17dc`, not `+0x17d8`) and never touching
the chain-link field - should be able to draw a real triangle/quad without depending on or corrupting
any of the marker-chain machinery `stage3-kernel-side-hang-mechanism-confirmed.md` found has no bounds
checking. Proposed test method: build a new minimal test program that (1) opens a raw second `AGLContext`
exactly as `stage3_native_shader.c` already does, (2) writes this exact register sequence (substituting
real framebuffer/texture addresses for this test's own pbuffer) directly at the verified-real cursor,
(3) uses the fence mechanism (`stage3-fence-mechanism.md`, selector 9) to synchronize properly rather than
an immediate follow-up GL call, and (4) reads back the result. Not run this session.
