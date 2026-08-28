# FFmpeg patch files

This project GPU-accelerates H.264 decode on a G5's ATI X1900 by hooking
FFmpeg's own `ff_h264_hl_decode_mb()` (see `x1900_hook.{h,c}`) rather than
registering a real AVHWAccel backend (see the project plan's "Architecture
correction" note for why).

These are the files modified/added against FFmpeg 7.1.5 (PowerFox's
`Jazzzny/powerfox-browser` AltiVec-patched base for G5/PowerPC), copied here
as full files (not a diff) for simplicity - the vendored FFmpeg tree itself
(131MB, mostly unmodified upstream source) is not tracked in this repo.

To rebuild: start from PowerFox's FFmpeg 7.1.5 AltiVec patch applied to a
real FFmpeg 7.1.5 checkout, then copy these files into place at
`libavcodec/{x1900_hook.h,x1900_hook.c,h264_mb.c,h264_mb_template.c,h264_loopfilter.c,h264_slice.c}`
before building. See the project plan (`~/.claude/plans/staged-beaming-platypus.md`
on the machine that has it, or ask - it's not itself committed here) for the
exact build/run commands.

Files:
- `x1900_hook.h` / `x1900_hook.c` - the hook itself: a plain-C function-pointer
  extension point with no FFmpeg-internal types, called from three sites
  below. Also owns `ff_x1900_hook_installed()`/`ff_x1900_set_postpone_wanted()`
  (item 9 frame-scale restructure) - lets the test harness tell FFmpeg's own
  decode loop whether to postpone deblocking for the current decode.
- `h264_mb.c` - one call site (`ff_x1900_call_mb_hook`, per-macroblock, right
  after entropy decode populates real coefficients/MVs/mb_type/QP and right
  before CPU reconstruction would run) plus debug tracing added over the
  project's history.
- `h264_mb_template.c` - real chroma/luma reconstruction reference code this
  project's own hook logic was cross-checked against during debugging
  (see the plan's chroma-DC bug write-ups).
- `h264_loopfilter.c` - the deblocking hook call site
  (`ff_x1900_call_deblock_hook`, M8).
- `h264_slice.c` - item 9's frame-scale restructure (2026-08-28): the
  `ff_h264_execute_decode_slices` single-slice-context path now optionally
  sets `h->postpone_filter` (an existing FFmpeg mechanism originally only
  used for multi-threaded slice decode) whenever the x1900 hook wants it,
  letting this project's own reconstruction defer/batch GPU dispatch across
  many rows instead of one row at a time - with a matching catch-up loop
  (mirroring FFmpeg's own multi-context catch-up code) that runs the
  deferred `loop_filter()` calls once decode_slice() returns. Restricted to
  non-I slices - see the plan's "Item 9: frame-scale restructure" write-up
  for the real, only-partially-traced I-slice interaction bug this was
  found necessary to work around.
