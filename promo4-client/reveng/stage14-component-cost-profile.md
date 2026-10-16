# Stage 14: component cost profile of the GL path (per component-cost-profiling method) - where the 35 ms/frame actually goes

Source: `~/rung3/h264/baseline1.log` on the G5 (jellyfish_360, 40 frames, DEBUG_GPU_PROFILE=1) divided by call counts, vs the repo's `finish-probe`/`fence-pipeline-probe` floors (G5, 2026-10-16).

| phase | calls | total ms | ms/call | note |
|---|---|---|---|---|
| IDCT batch: pack | 437 | 53.5 | 0.12 | CPU |
| IDCT batch: upload | 437 | 26.9 | 0.06 | CPU |
| **IDCT batch: draw+glFinish** | 437 | 111.1 | **0.254** | trivial-shader draw+finish floor = **0.239** |
| IDCT batch: readback+unpack | 437 | 148.8 | 0.34 | small glReadPixels floor ~0.43 (400x16) |
| MC singlepass: pack+upload | 804 | 16.7 | 0.02 | |
| **MC singlepass: draw+glFinish** | 804 | 225.7 | **0.281** | floor 0.239 |
| MC singlepass: readback+unpack | 804 | 277.5 | 0.345 | |
| lumadc | 165 | 113.8 | 0.69 | |
Total GPU-dispatch time per 40 frames = 1522 ms (wall); CPU time inside = 1146 ms (75%).

## Reading
- The real shaders' draw+finish is within ~5-15% of an EMPTY draw+finish. GPU shader execution is not the bottleneck at this batch size; the fixed per-round-trip cost is (finish ~0.24 ms + readback ~0.35 ms + pack/upload ~0.1-0.2 ms ~= 0.7-1.0 ms per round trip), and there are ~25 round trips per frame.
- Consequence for PROMO4: (a) hand-written native shaders would shave the small compute share only. The stock compiler's output for the real shaders is long (captured: IDCT 203 instructions incl. 39 flow-control; MC 117; diag-MC 170; lumadc 181) but vectorising the GLSL (dot()/vec4 packing) would give the same compaction without the raw route. (b) Raw PM4 does not by itself remove the sync/readback cost. (c) What can still pay off: a cheaper fence (poll the retire stamp instead of glFinish), GPU->GART copy instead of glReadPixels (driver CPU is 70% of readback), and fewer round trips (batch across the frame).
- Amdahl: removing readback entirely would take 35 -> ~23 ms/frame; AltiVec is 6.3 ms/frame. Beating it needs the round-trip COUNT to drop by roughly an order of magnitude, i.e. restructuring the decoder so intra prediction / deblock / reference management stay on the GPU or are batched per frame.

## Experiment: is there a client-visible completion stamp to poll instead of glFinish? (stage3m / stage3n) - YES (first conclusion here was wrong)
- Read-only probe `stage3m`: type 2 (32 KB shared) never changes across draws. In the type-0 mapping (4 KB), the little-endian word at +0x0 (mirrored at +0x20) advances with every submission.
- First single-shot test (word changed 0.22 ms after `glFlush`, long before the 15 ms job finished) led to the WRONG conclusion "submit counter only". The time-resolved TIMELINE run (3 repeats) corrects it: the word changes TWICE per flush - at ~0.07-0.14 ms (flush return) and again at 14.89-14.93 ms, which is the job's completion (reference draw+glFinish = 14.97-15.04 ms). So it IS a completion stamp, +1 per retired command buffer, and a flush of this workload retires 2 (a small preamble buffer and the main buffer).
- Benchmark (`stage3n`, trivial clear+quad, 300 reps x3): `glFinish` **0.130 ms/rep** (CPU 0.029) vs `glFlush` + spin until stamp advanced by 2 **0.077 ms/rep** (CPU 0.077 = busy spin), -41% wall. Heavy 15 ms job (30 reps): glFinish 14.910 vs poll 14.853 ms (CPU 14.85 spinning vs 0.46).
- Caveats: the target (+2 per flush) is calibrated per command-structure, not a documented tag (kernel's retire tag is not client-visible); a wrong count would hang the spin, so any real use needs a bounded loop with a `glFinish` fallback. Spinning burns a full core (glFinish yields: 22% CPU).
- Value for the decoder: ~0.05 ms saved per round trip x ~25 round trips/frame = ~1.3 ms of ~35 ms/frame. Real but small; it does not change the conclusion below.
