# Stage 4: full autonomous decompilation sweep, hardware-up

Continuing at the user's explicit direction to work autonomously across all downloaded driver
binaries, applying the register-doc cross-reference method systematically, starting from real
hardware/MMIO interactions and working up the stack. Per the user's standing instruction for this
sweep: any hypothesis that would need real hardware to test is documented here with its proposed test
method, and NOT attempted - no G5 access this session regardless.

## `ATIRadeonX1000::submit_ring_data`/`submit_buffer`/`submit_buffer_retired` - the true bottom of the stack, fully confirmed

Decompiled the base hardware class's real ring-buffer submission path (`0x1f030`/`0x20980`/`0x20700`).

**`submit_ring_data`** is the literal MMIO ring-pointer nudge: after real PowerPC cache-coherency
handling (`dataCacheBlockStore`/`dataCacheBlockFlush`/`sync`/`instructionSynchronize`/
`enforceInOrderExecutionIO` - genuine memory-barrier code ensuring the ring's CPU-written bytes are
visible to GPU DMA before advancing the pointer), it does:
```c
*(uint *)(mmioBase + 0x714) = wptr << 0x18 | (wptr & 0x700) << 8;
```
`0x714` = **`CP_RB_WPTR`** (Command Processor Ring Buffer Write Pointer) - confirmed exactly against
`r5xx_accel_v15.txt`'s real Command Processor Registers section (11.1), independently re-confirming a
past session's finding from a different angle.

**`submit_buffer`** constructs a real, standard AMD **indirect-buffer submission** sequence. Applying
the same Type-0-header decode already validated in `stage3-write-kernel-context-buffer-regs-fully-
decoded.md`, the literal constant `0x101ce` decodes to `TYPE=0, COUNT=2, BASE_INDEX=0x1ce` → byte
address `0x738`. **Confirmed exactly**: `CP_IB_BASE` (`0x738`), with `CP_IB_BUFSZ` (`0x73c`)
immediately following - and the two data dwords written right after the header are literally the
buffer's address and size. This is genuine, textbook "submit an indirect buffer via the ring" - the
ring itself just carries a tiny header pointing at the real, larger command buffer elsewhere in memory
(this project's actual command buffers, already extensively analyzed). The FIFO-space retry loop
(`if (... < 5) { retry up to 1000 times }`) matches exactly: 5 dwords is the real size of this specific
header+addr+size(+pad) sequence.

**`submit_buffer_retired`** is the same mechanism plus real fence/completion bookkeeping: after the
same indirect-buffer header, it appends a **`0x394`**-indexed single register write (decodes to byte
`0xe50`, immediately after the already-confirmed `RBBM_STATUS` block at `0xe40-0xe43` - not found by
name in the currently-extracted doc sections, plausibly a scratch/general-purpose "write a value the
CPU can poll" register in that immediate neighborhood) with the current retirement counter
(`this+0x850`) as the value, then a `0x80000000` PM4 Type-2 filler dword. **This is almost certainly
the real mechanism behind the fence/timestamp completion counter** already found client-side in
`stage3-fence-mechanism.md` (`AGLContext+0x1838`'s live completion counter) - the kernel writes the
current buffer's retirement tag directly into a scratch register/memory location the GPU's command
stream execution reaches only after truly finishing everything before it, and userspace polls that
same location. Two more constants (`0x578`/`0x579`, bytes `0x15e0`/`0x15e4`) appear in
`submit_buffer`'s slower/retry path and are not yet confirmed against a named register - flagged
honestly, not guessed.

**Testable hypothesis, not attempted (no hardware this session)**: if `0x394` (byte `0xe50`) really is
the fence-completion scratch register, live-reading it via a `finish-probe`-style program immediately
after a known buffer retirement, cross-referenced against the already-confirmed `AGLContext+0x1838`
counter, should show the two values tracking identically. Test method: build a small program that maps
the kext's memory type 0 (already known to be a 4KB status/register region) or reads the live
`AGLContext` field directly, submits a trivial buffer, and polls both locations to confirm they move
together. Not run - flagging for whenever the G5 is back.

Committed as this sweep continues - more sections below as further functions are decoded.
