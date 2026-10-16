# Stage 15: resolution scaling of the current GL-path design (720p)

Clip: `jelly_720.mp4` = ffmpeg `-vf scale=1280:720 -frames:v 60 -c:v libx264 -profile:v high -preset medium -crf 23` of `testdata/jellyfish_360.mp4`. Run on the G5: `MULTI_FRAME=30 DEBUG_GPU_PROFILE=1 ./gpu-live-decode-test jelly_720.mp4` (log: `stage15-live-720p-baseline.log`).

| | 360p (640x360) | 720p (1280x720) | ratio |
|---|---|---|---|
| pixels | 230,400 | 921,600 | 4.0x |
| AltiVec CPU ms/frame (steady) | 6.3 | **19.3** | 3.1x |
| GL-path ms/frame (steady) | 35.4 | **207.9** | 5.9x |
| GL / CPU | 5.6x slower | **10.8x slower** | |
| flush_pending round trips per frame | ~10 (415/40) | **~122 (3659/30)** | 12x |
| readback+unpack share of GPU-dispatch time | ~33% | ~42% | |

The MB-level hook issues round trips in proportion to macroblock count, so the GL path scales WORSE than AltiVec with resolution. Higher resolution does not rescue the current architecture; at 720p AltiVec is already real-time (52 fps).

Not measured yet and needed to bound any redesign: the CPU-side breakdown at 720p (entropy decode vs reconstruction vs deblock). Only the GPU-offloadable share (IDCT+MC+deblock) can shrink; entropy decode stays on the CPU, so that share is the upper bound on any possible gain.
