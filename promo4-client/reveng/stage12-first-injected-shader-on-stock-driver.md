# Stage 12: FIRST SUCCESS - a hand-assembled fragment program, injected as raw PM4 into the stock driver's live command stream, executes on the GPU

Issue #1. Stock, unmodified kext and GL driver. Date 2026-10-16, G5 (Tiger 10.4.11, X1900).

## Result (live, repeated)
Workload: GLSL red fragment shader, 4x4 pbuffer, `glClear` then a quad. Baseline pixel `255,0,0,255`.
- A3: inject 4 dwords (2 Type-3 NOPs) -> pixel unchanged, context healthy afterwards.
- **A4: inject the 11-dword program-load PM4 for the hand-assembled BLUE program (US slot 100, `US_CODE_ADDR` -> slot) -> the same quad renders `0,0,255,255`.** Repeated 4x, always blue.
- Control: inject the RED program -> `255,0,0,255` (matches baseline). Post-check clear (green) renders correctly after every run; no hang, no panic.
Logs: `stage11-live-A1.log` (stage2c, independent client), `stage11-live-s3j_A3.log`, `stage11-live-s3j_A4.log`.

## Why it worked this time (the stage 3g hang)
Stage 3g advanced ctx+0x17d8 (the CHAIN LINK to the pending record header) instead of the write cursor ctx+0x17dc, corrupting the length chain the kernel walks. The read-only probe `stage3i` confirmed the corrected map on live data (link = pending record header, length 0 until flush; cursor = next free dword; limit; base) and `stage3j` writes ONLY the cursor. The words land inside the body of a pending `0x2f` record, which the stock kernel passes through to the ring (it rewrites only that record's header to a Type-2 filler and patches body words 2 and 4).

## The pipeline that made the first live injection safe (all before touching the G5)
1. `tools/promo4_sim/pm4check.py` + `gate.py`: packet structure + register policy.
2. `dryrun.py`: stock `process_command_buffer` in the PPC emulator; framing verified against a real captured buffer.
3. `inject_sim.py` / `inject_sim2.py`: replay REAL captures of the exact workload (pre-draw and post-draw, taken read-only) with our words inserted at the live cursor; the kernel's output before/after the insertion is identical to the control and our words appear verbatim.
4. `r5xx_us.py`: interpreter reproduces the same two programs' RGB output (red/blue) from their instruction words.
The live state matched the simulated capture to the byte offset (link +0x558, cursor +0x57c), twice.

## What this does and does not prove
Proves: arbitrary PM4 (here a US program load) can be delivered to the GPU through the stock driver with no kext changes, with the driver's own render target/state in place. Does not yet prove a performance win: the next questions are per-submission cost vs the GL path, reading results back cheaply, and running the real IDCT/MC programs (texture fetch + arithmetic) instead of a constant-colour program.

## Update (stage 13): native TEXTURE-SAMPLING program works too
Captured the stock compiler's output for three texture shaders (`stage13-capture-texshader-v{1,2,3}.bin`, decoded by `tools/promo4_sim/decode_us.py`): passthrough = 2 instructions (`TEX` + `OUT`), `*2+0.25` = 2, 4-tap average = 8 (4 TEX, 3 ALU, 1 OUT). Program loads: `US_VECTOR_INDEX`(0x4250)=slot, `US_VECTOR_DATA`(0x4254) one-reg-write of 6 dwords/instruction, `US_CODE_RANGE`(0x4634) = window start | (size-1)<<16, `US_CODE_ADDR`(0x4630) = start | end<<16. TEX word layout confirmed on hardware data (TEX_INST: INST=1<<22 LD, SEM_ACQUIRE bit25, IGNORE_UNCOVERED bit26, UNSCALED bit27 for rectangle textures; TEX_ADDR: swizzles in 9:8/11:10/13:12/15:14, DST in 22:16, DST swizzles 25:24..31:30).
Live (G5, stock driver): bound GLSL = texture2DRect passthrough; injected the compiler's own 2-instruction program with the OUT RGB swizzles reversed. Result: all 16 pixels = (B,G,R,A) of the texel, exactly the interpreter's prediction (pixel(1,1) 127,63,63,255 vs baseline 63,63,127,255); the unmodified passthrough control left all 16 unchanged; repeat identical; post-check clear OK. Log: `stage13-live-texswap.log`.
