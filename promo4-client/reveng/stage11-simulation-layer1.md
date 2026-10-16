# Stage 11: simulation layer 1 - the stock kernel's command-stream framing, verified in the emulator (zero hardware risk)

Tracking issue: #1. Tool: `tools/promo4_sim/dryrun.py` (drives kext-source `Tools/userspace/emu/kemu.py`, which runs the STOCK
`ATIR500GLContext::process_command_buffer` machine code under unicorn).

## Finding: the buffer is NOT raw PM4 - it is length-prefixed records
`process_command_buffer` (source: `ATIR500GLContext_process_command_buffer_Port.cpp`, loop at ~L203, advance at L3179) starts at
`BUF+0x1c` and reads a header word per record: `opcode = hdr>>24`, `length = hdr & 0xffffff` dwords (header included). After a
record it advances `length` dwords; `length == 0` terminates. Handled opcodes are rewritten in place to a Type-3 NOP
(`(N-2)<<16 | 0xc0001000`) or Type-2 filler so the GPU skips them.

Earlier docs (stage2b) filled the payload with bare `0x80000000` and read this as "raw PM4 accepted". Under this framing the
first word at `BUF+0x1c` was a Type-2 value `0x80000000` = opcode 0x80, length 0, i.e. an immediate terminator - the test was
safe but did not exercise a real PM4 payload. A bare Type-0 header such as `0x00010400` would be read as opcode 0 with
length 0x10400 dwords and walk 262 KB past the start (emulator: `cur = BUF+0x41020`) - a real over-read hazard.

## Verified pass-through form (the one the real driver uses: opcode 0x00 = 5298 of 5353 recorded GL flushes)
`[ n+1 ][ pm4 word 0 .. n-1 ][ 0 ]` placed at `BUF+0x1c`:
- header (opcode 0) is NOT rewritten and sits at `BUF+0x1c`, outside the submitted range;
- submit range = `[BUF+0x20, +4*size)`, `size = n` (+1 if n is odd; the kernel appends one `0x80000000` pad);
- the PM4 words come through byte-identical (emulator, 3 cases).
Hazard: a *second* opcode-0 record inside one flush would leave its header word inside the submitted range, unrewritten, and the
GPU would read it as PM4. Use exactly one pass-through record per flush.

## Tool checks (`dryrun.py`)
Emulator status RET; no unmapped-memory touches (the hang class); no calls outside the kext image; submit range starts at
BUF+0x20; submitted words == intended PM4 (+pad); record length within the REAL 128 KB buffer capacity (32760 dw).

## Not covered by layer 1
What the GPU does with the words (register effects, engine wedge, DART faults). That is layers 2-4 and the staged live run.

## Validation against a REAL stock GL buffer (2026-10-16, G5, read-only recorder run)
`DYLD_INSERT_LIBRARIES=opcode_recorder.dylib OPCODE_DUMP=6` on the stock `cgl_probe` (capture committed beside this file). First flush, buffer 0x20000:
```
+0x00..0x0c  0 0 0 0           (header, zeroed)
+0x10        0x00007c52        (capacity/cursor-ish field, not used by the framing)
+0x1c        0x0000010b        <- first record header: opcode 0, length 267 dwords, OUTSIDE the submit range
+0x20        00001181 00000000 00001189 00000000 00001188 00000000 0000118c 00000000 ...   <- PM4: Type-0 count-0 (reg index, value) pairs
```
This is exactly the layer-1 model: one opcode-0 record at +0x1c, PM4 from +0x20 as Type-0 `(index,value)` pairs (0x1181 = US_PIXSIZE, 0x1189 = US_FC_CTRL, 0x118c = US_CODE_ADDR ...). The second capture starts with a 1-word record at +0x1c (`0x00000001`, what `init_command_buffer_header` leaves) followed by a special opcode (`0x28000009`) at +0x20 that the kernel handler rewrites. Both match the emulator's walk.

## Layer 3 (R5xx fragment ISA interpreter) - `tools/promo4_sim/r5xx_us.py`
Decoder + single-pixel interpreter from the AMD R5xx doc v1.5 field tables. Ground truth: it reproduces, from the same instruction words, the two programs `stage3_native_shader.c` ran on real hardware (red (1,0,0,1), blue (0,0,1,1)); `test_r5xx_us.py` also checks MAD, swizzle/inline-constant decode (the doc's "1.0" table row is garbled in extraction: with bias 7, 1.0 is exp 7 = 0xB8 as an address byte). Unsupported (raises, never guesses): flow control, predication, relative addressing, MDH/MDV, texture-semaphore timing. float32 arithmetic; hardware is FP24 in places, so a match proves logic, not bit-exact precision.
