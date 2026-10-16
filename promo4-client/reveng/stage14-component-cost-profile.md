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

## Experiment: is there a client-visible completion stamp to poll instead of glFinish? (stage3m / stage3n) - NO
- Read-only probe `stage3m`: the accelerator-wide shared region (memory type 2, 32 KB) never changes across draws. The type-0 mapping (4 KB) has little-endian words at +0x0 and +0x20 that advance by 2 per draw+finish cycle.
- `stage3n` microbenchmark: trivial draw+`glFinish` = 0.129 ms/rep (CPU 0.029); draw+`glFlush`+poll of that word = 0.018 ms/rep. Too good: with a deliberately heavy job (full draw+glFinish = 15.1 ms) the word changed 0.219 ms after `glFlush` returned (flush itself 0.218 ms), i.e. at SUBMIT time, with 0.239 ms of the remaining glFinish still pending after... so it is a submit counter, not a retirement stamp.
- Kernel source (`ATIRadeonX1000::waitForRetiredTimeStamp` / `Start`): the retire stamp is `(this+0x864)+(this+0x868)`, either the MMIO scratch register 0x15e4 or a kernel IOMallocAligned writeback page at +4 - neither is exposed to clients through memory types 0/2. A user-space polled fence therefore cannot be built on the stock driver without a kext change (out of scope) or a GPU-written fence into client memory (needs a GPU-visible address and a packet that stores a value: not yet researched).
- Floor stays: sync ~0.13-0.24 ms per round trip.
