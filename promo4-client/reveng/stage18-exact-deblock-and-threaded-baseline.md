# Stage 18: exact deblock cannot be done on the GPU, and the real CPU baseline is multi-threaded

## 1. Exact H.264 deblock is a serial chain, not a data-parallel pass
The spec filters macroblocks in raster order, each MB doing all vertical edges then all horizontal edges.
- Luma bS<4 vertical edges 4 px apart: edge at x reads p2 = x-3, which the previous edge (x-4) wrote as q1. So V0->V4->V8->V12 is a chain.
- H(MB n) modifies columns 13..15 of MB n, and V0 of MB n+1 reads/modifies exactly those columns. So H(n) -> V0(n+1) -> V4 ... -> H(n+1): the chain runs through the whole MB row (120 MBs at 1080p).
- Rows couple with a lag of 2 MBs (H0 of MB (x,y) reads what V0 of (x+1,y-1) wrote), so the best wavefront is t = x + 2y: 256 waves at 1080p.
- "All V edges of the picture, then all H" (the HEVC formulation) is NOT equivalent for H.264, which is why the stage-17 stand-in was not bit-exact.
- Chroma: V edges (4 samples apart, p0/q0 only modified) are mutually independent, but the H(n)->V0(n+1) coupling still serialises a row.
256 waves x (>=3 draws, no in-place read/write allowed -> ping-pong/copy) at ~50-250 us per draw is >40 ms before any ALU. Not viable. Deblock must run on the CPU (or be approximate, which the success criteria forbid).

## 2. Cost of the GPU-recon + CPU-deblock hybrid (src/xfer_bench.c, G5, stock driver)
Readback goes through a sampling pass to the pbuffer (quirk 6). 1080p 4:2:0 frame packed 4 px/texel RGBA8 = 3.1 MB:
| | ms |
|---|---|
| upload glTexSubImage2D RGBA/UBYTE 3.1 MB | 3.3 (BGRA/8888_REV is slower: 7.3) |
| sampling pass | 0.5 |
| glReadPixels 3.1 MB | 6.3 (BGRA) / 8.0 (RGBA) |
Round trips are byte-exact. Serial chain per reference frame: GPU recon ~18 + read 6.3 + CPU deblock 9 + upload 3.3 = ~37 ms -> no better than AltiVec single thread (39.3 ms), and it only reaches 30 fps with band-level pipelining and perfect balancing.

## 3. The decisive number: the CPU baseline scales with threads
src/decode_stage_timing.c now takes THREADS=n (FFmpeg frame threading; TTYPE=1 = slice threading). G5 is a quad (hw.ncpu 4), AltiVec, stock FFmpeg 7.1.5 + our hook disabled:
| clip | 1 thread | 2 | 4 |
|---|---|---|---|
| 720p | 16.9 ms | | 5.0 ms (200 fps) |
| 1080p | 39.3 ms | 20.9 ms | **11.9 ms (84 fps)** |
Slice threading gives nothing (39 ms: the stream has one slice per picture). Frame threading gives 3.3x at 4 threads.
(Clips are upscaled from 360p so they are easier than native 1080p content; relative scaling is what matters here.)

## Consequence
The revised goal on #1 (1080p High >= 30 fps bit-exact) is already met on this machine by plain threaded AltiVec decode at 11.9 ms/frame. A GPU pipeline would have to beat ~12 ms/frame with an entropy+deblock floor of ~22 ms single-threaded CPU work per frame that cannot move to the GPU. It cannot win on throughput. The only remaining GPU value is offloading MC+residual to free CPU cores (CPU load, not speed), and that costs more bus traffic than it saves unless deblock is skipped.
