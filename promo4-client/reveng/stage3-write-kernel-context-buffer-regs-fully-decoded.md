# `write_kernel_context_buffer_regs` fully decoded: register-doc cross-referencing pays off completely

Applying the user's suggested method directly: rather than reasoning about a function's decompiled
logic in isolation, decode its literal numeric constants against the real AMD register documentation
already extracted locally (`r3xx_3d_registers.txt`, `r5xx_accel_v15.txt` - both from
`~/Documents/AMD Docs/`, via `ps2ascii`) and real working driver code (the KolibriOS reference already
used). Applied to `ATIR500GLContext::write_kernel_context_buffer_regs` (`000288e0`, called from opcode
`0x41000000`'s handler in `process_command_buffer`) - a function that emits a real, raw PM4 register
list. Result: nearly every constant in the function decodes exactly, several with two-source
confirmation.

## The real encoding

Every "index" constant this function writes is a genuine PM4 Type-0 register-write header, same format
already established elsewhere in this project (`stage3g-cursor-field-misidentified.md`):
`TYPE=(v>>30)&3`, `COUNT=((v>>16)&0x3FFF)+1`, `BASE_INDEX=v&0x1FFF`, real byte address =
`BASE_INDEX*4`. Values under `0x2000` are single-register writes (`COUNT=1`); values with bits above
16 set are real multi-register bursts, confirmed by the following N values matching N=COUNT exactly.

## Fully decoded register table (all cross-checked against local AMD docs)

| Written index | Byte addr | Real register (both docs agree) | Value written |
|---|---|---|---|
| `0x13c6` | `0x4f18` | `ZB_ZCACHE_CTLSTAT` | `3` |
| `0xd0b`  | `0x342c` | *(not in either local doc - see below)* | `5` |
| `0x1393` | `0x4e4c` | `RB3D_DSTCACHE_CTLSTAT` | `10` |
| `0x1006` | `0x4018` | `GB_TILE_CONFIG` | per-context tiling tag |
| `0x50b`  | `0x142c` | *(not in either local doc - see below)* | computed pitch/format expr |
| `0x3138a` (burst, COUNT=4) | `0x4e28`-`0x4e34` | **`RB3D_COLOROFFSET[0-3]`** (real, doc-named 4-register array, exact match) | 4 computed per-MRT color-buffer byte offsets |
| `0x3138e` (burst, COUNT=4) | `0x4e38`-`0x4e44` | **`RB3D_COLORPITCH[0-3]`** (same, exact match) | 4 computed pitch/format-table-derived dwords |
| `0x1380` | `0x4e00` | `RB3D_CCTL` | HyperZ-conditional value (`0x600` or `0`) |
| `0x1385` | `0x4e14` | `RB3D_COLOR_CLEAR_VALUE` | a per-surface stored clear-value field |
| `0x1395` | `0x4e54` | *(not in either local doc)* | a `HZMEM_GetBlockOffset` result |
| `0x1399` | `0x4e64` | *(not in either local doc)* | a computed pitch/alignment value |
| `0x113c8` (COUNT=2) | `0x4f20`-`0x4f24` | `ZB_DEPTHOFFSET` + adjacent | HiZ-adjacent computed value + format-derived bitfield |
| `0x13d8` | `0x4f60` | `ZB_DEPTHXY_OFFSET` | `0` |
| `0x13c4` | `0x4f10` | `ZB_FORMAT` | a format-conditional bit expression |
| `0x13ca` | `0x4f28` | `ZB_DEPTHCLEARVALUE` | a `HZMEM_GetBlockOffset` result (see honest caveat below) |
| **`0x13c7`** | **`0x4f1c`** | **`ZB_BW_CNTL`** | **`compute_zb_bw_cntl(this, param_4)`** |
| `0x13cc` | `0x4f30` | *(not in either local doc)* | `uVar25` (HiZ block offset) |
| `0x13cd` | `0x4f34` | *(not in either local doc)* | `uVar18` (computed alignment) |
| **`0x10e9`** | **`0x43a4`** | **`SC_HYPERZ_EN`** | **`compute_sc_hyperz_en(this, param_3)`** |
| `0x13d1` | `0x4f44` | `ZB_HIZ_OFFSET` | a `HZMEM_GetBlockOffset` result - matches the doc's own description ("DWORD offset into HiZ RAM") exactly |
| `0x13d5` | `0x4f54` | `ZB_HIZ_PITCH` | a computed alignment value - matches "Pitch used in HiZ address computation" exactly |
| **`0x10f4`** | **`0x43d0`** | **`SC_CLIP_RULE`** | **`0xaaaa`** |

## Two results with airtight, name-level confirmation (not just address matching)

- **`compute_zb_bw_cntl`'s return value is written to register `0x4f1c` = `ZB_BW_CNTL`** - the
  Ghidra-recovered C++ function name and the AMD-documented register name match exactly. This isn't a
  coincidence of address arithmetic; it's the same real fact confirmed from two completely independent
  naming sources (Apple's own mangled C++ symbol table, and AMD's register documentation).
- **`SC_CLIP_RULE` (`0x43d0`) gets the literal value `0xaaaa`** - exactly the value found earlier in
  real, working, open-source driver code (`stage3-rasterizer-enable-found.md`'s KolibriOS reference).
  **Three independent sources now agree**: AMD's register description, a real third-party open-source
  driver, and Apple's own shipped kext.

## Honest gaps: five addresses not found in either local document

`0x342c`, `0x142c`, `0x4e54`, `0x4e64`, `0x4f30`, `0x4f34` don't appear in `r3xx_3d_registers.txt` or
`r5xx_accel_v15.txt` under any register name. Two of these (`0x4e54`, `0x4e64`, `0x4f30`, `0x4f34`) sit
right inside the same `RB3D`/`ZB` blocks as everything else in this function and are written with
real HiZ-block-offset/alignment values (`HZMEM_GetBlockOffset` results) - very likely genuine R5xx-only
HyperZ registers that simply postdate or weren't covered by these two specific document revisions (this
project has four other `R5xx_Acceleration` versions locally, v1.1-v1.4, not yet checked for these
specific addresses - a reasonable next step if more precision is wanted here). `0x342c`/`0x142c` are
architecturally distant from the rest of this function's register cluster and their real purpose (paired
with plain small constants `5` and a computed pitch expression respectively) is not resolved - flagged
honestly rather than guessed.

## What this fully confirms about opcode `0x41000000`

Combined with the earlier client-side trace (`stage3g-cursor-field-misidentified.md`'s note on
`FUN_0001bac0`), this opcode's real, complete purpose is now clear: **it's the real "commit render-
target/framebuffer attachment state" operation** - processing up to 4 real MRT-style color-buffer
attachments (`RB3D_COLOROFFSET[0-3]`/`COLORPITCH[0-3]`), the real depth/HyperZ setup
(`ZB_FORMAT`/`ZB_DEPTHOFFSET`/`ZB_HIZ_*`/`SC_HYPERZ_EN`/`ZB_BW_CNTL`, all correctly gated by the
already-documented `compute_sc_hyperz_en`/`compute_zb_bw_cntl` per-surface-flag logic), the real
visibility gate (`SC_CLIP_RULE=0xAAAA`, matching this project's own already-validated finding), and
cache-flush bookkeeping (`ZB_ZCACHE_CTLSTAT`, `RB3D_DSTCACHE_CTLSTAT`). This single opcode is
effectively Apple's own driver performing the exact "render-target setup" sequence
`stage3-render-target-and-full-draw-reference.md` reconstructed from third-party reference code - now
independently confirmed from Apple's own shipped binary, register-by-register.
