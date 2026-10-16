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
