# Stage 16: CPU stage budget of the AltiVec decoder at 360p / 720p / 1080p - what a GPU offload can possibly buy

Tool: `src/decode_stage_timing.c` (built on the G5 with gcc-7 + ld64 against the patched FFmpeg 7.1.5 static libs; PATH=/usr/local/opt/ld64/bin:/usr/local/bin:$PATH, `-std=gnu99`). Stages via env STAGE: `full` = normal AltiVec decode; `nodeblock` = `skip_loop_filter=AVDISCARD_ALL`; `entropy` = deblock skipped AND a null per-MB hook that "takes over" every macroblock (so no pixel reconstruction): bitstream/CABAC parsing, MV prediction, bookkeeping only. Single thread, 3 repeats (1080p: 2), numbers stable to <1%.
Clips: 360p = `testdata/jellyfish_360.mp4` (300 frames); 720p/1080p = ffmpeg libx264 High crf23 scaled from it (60 frames each; not committed).

| ms/frame | full | no-deblock | entropy+parse only | => entropy | recon (MC+IDCT+intra) | deblock | offloadable | fps (full) |
|---|---|---|---|---|---|---|---|---|
| 360p  | 5.16  | 4.14  | 1.55  | 1.55 (30%) | 2.59 (50%) | 1.02 (20%) | 70% | 194 |
| 720p  | 16.65 | 13.15 | 5.73  | 5.73 (34%) | 7.42 (45%) | 3.50 (21%) | 66% | 60 |
| 1080p | 39.3  | 29.9  | 12.86 | 12.86 (33%) | 17.05 (43%) | 9.0 (23%) | 67% | **25.4** |

## What this means
- Entropy decode + parsing (~1/3) must stay on the CPU. A perfect, free, fully overlapped GPU therefore caps the speedup at ~3x (1080p: 39.3 -> 12.9 ms/frame).
- AltiVec is real-time at 360p/720p. At **1080p it is not (25.4 fps)**: that is where GPU assist has user-visible value. The target is not "beat AltiVec outright" but "make 1080p30 real-time": GPU stage budget ~33 ms/frame pipelined, CPU needs only ~13 ms/frame.
- The current macroblock-level design cannot get there (720p: 10.8x slower; ~122 round trips/frame). A frame-level design needs O(1) round trips per frame, output presented from the GPU (a 1080p readback via glReadPixels would eat the budget), and a plan for intra prediction (sequential dependency) - e.g. CPU-side for intra MBs, or wavefront passes.
