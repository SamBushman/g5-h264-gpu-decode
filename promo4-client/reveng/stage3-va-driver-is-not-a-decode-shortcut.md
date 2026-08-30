# The VA driver is a display/overlay path, not a codec-decode shortcut - an honest negative result

Follow-up to `stage3-kernel-side-hang-mechanism-confirmed.md`, investigating `ATIRadeonX1000VADriver
.bundle` (copied from the locally-mounted Tiger HD) since it was flagged as "possibly relevant to the
project's broader H.264-decode goal" - worth checking directly rather than assuming.

## Real API surface: `AVA*` ("Apple Video Acceleration")

Only four real named entry points exist: `_AVACreateRenderer`, `_AVACreateRendererDisplayExt`,
`_AVACreateRendererDVDExt`, `_AVAGetRendererInfo`. Decompiled all three creation functions - they are
thin object-construction code: allocate a small struct, install a version tag (`0x1020000`), a
capability bitmask, a name string (`"ATIVADriver "`), and populate a dispatch table with ~20 function
pointers. No codec-specific setup of any kind in these top-level entry points.

## Real, honest evidence this is a display/overlay/colorspace-conversion path, not a decoder

- **Zero codec-specific symbol names anywhere in the whole binary** - searched exhaustively for
  `mpeg`/`motion`/`macroblock`/`idct`/`deblock`/`h264`/`codec`/`vld`/`bitstream` (case-insensitive) in
  both the symbol table and raw extracted strings. Nothing.
- **Real string fragments found**: `=yuv2`/`=yuv2t` - consistent with a YUV-to-something (RGB) color-
  space conversion table, not a bitstream decoder.
- **The API's own naming**: `AVACreateRendererDisplayExt`/`AVACreateRendererDVDExt` - "Display" and
  "DVD" *extensions* to a renderer, i.e. presentation-time hardware assistance (scaling, overlay,
  color conversion) for **already-decoded** video frames, not decode acceleration itself.
- **Historical/architectural context**: this exact bundle-naming pattern (`ATI<GPU>VADriver.bundle`)
  exists identically across every ATI GPU generation on this Tiger install, confirmed by directory
  listing - including `ATIRagePro`, `ATIRage128`, `ATIRadeon` (original), `ATIRadeon8500` - GPUs from
  1999-2001, years before any consumer GPU did real bitstream motion-compensation/IDCT decode. A stable
  API surface spanning that whole hardware range is architecturally far more consistent with "hardware
  overlay and YUV colorspace conversion" (a capability GPUs of that era genuinely had) than "hardware
  video decode" (which they did not).

## Honest, hedged note on one dense function

Decompiled a few of the ~30 `caseD_*` switch-case handlers behind the dispatch table (e.g. the function
at `00008820`). These are dense, and involve bit-patterns (paired 16-bit values packed via
`(x - (x>>31))*0x8000 & 0xffff0000 | ...`) that *could* be read as motion-vector-style fixed-point
packing, and per-index queue bookkeeping that *could* be read as per-macroblock reference tracking.
**Not confidently resolved either way** - this pattern is equally consistent with scaling-filter tap
coordinates or scanline/tile position bookkeeping for hardware overlay scaling, which fits the rest of
the evidence far better than motion compensation would. Flagging honestly as unresolved rather than
asserting a specific interpretation - would need substantially more decompiling of the full case-handler
set and its callers to pin down precisely, and the payoff looks low given how one-sided the rest of the
evidence already is.

## Conclusion

**This driver is very likely not a shortcut to hardware-accelerated H.264 bitstream decode** - it looks
like the display-side hardware overlay/scaling/YUV-conversion path DVD Player used for presenting
frames a *separate, software* MPEG-2 decoder had already produced, consistent with this era's real
Mac hardware capabilities. This doesn't change this project's own approach (GPU-shader-accelerated
reconstruction stages via FFmpeg's hwaccel API) - if anything, it confirms there was no simpler,
Apple-sanctioned decode-acceleration path being overlooked. Not pursued further given the one-sided
evidence and low expected payoff of resolving the remaining ambiguous function.
