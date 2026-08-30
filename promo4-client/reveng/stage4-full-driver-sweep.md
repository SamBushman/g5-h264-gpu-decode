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

## `initialize_hardware`/`setup_R500_internal_space`/`setupR520Pipes` - real hardware bring-up, many more registers confirmed

Decompiled the three real hardware-bring-up functions `initialize_hardware` calls in sequence
(`0x1f3c0`/`0x1cf70`/`0x1bd20`). Batch-checked every literal MMIO offset against the local docs:

**Confirmed exactly**: `VAP_CNTL` (`0x2080` - same register the KolibriOS reference code writes),
`VAP_PVS_STATE_FLUSH_REG` (`0x2284`), `CP_RB_BASE` (`0x700`), `CP_CSQ_CNTL`/`CP_CSQ_MODE` (`0x740`/
`0x744`), `GB_PIPE_SELECT` (`0x402c`), `SU_REG_DEST` (`0x42c8` - matches `R500_SU_REG_DEST` in the
KolibriOS reference exactly), `GB_TILE_CONFIG` (`0x4018`, written here with a real pipe-count-derived
value during `setupR520Pipes` - the same real init-time write the KolibriOS reference performs),
`GA_SOFT_RESET` (`0x429c`), `SC_CLIP_RULE` (`0x43d0`, written `0xffff0000` here at hardware-init time -
a different value from the per-draw `0xaaaa` already confirmed, consistent with a startup default
before real rendering begins).

**Real, structurally-confirmed pattern, not yet name-matched**: `setup_R500_internal_space` repeatedly
writes an "index" to `mmioBase+0x30` immediately followed by a "data" value to `mmioBase+0x34` - the
exact shape of an indirect index/data register-access pair (like `GA_US_VECTOR_INDEX`/`_DATA` already
used elsewhere in this project, but for a different block - very likely a real Memory Controller
indirect access pair, common on this GPU family for registers not otherwise memory-mapped). Real index
values seen: `0x10000000`-`0x17000000` (top-byte-tagged, matching the "one index dword selects one MC
register" convention) with GART/memory-range-derived data (base addresses, sizes).

**`setupR520Pipes`'s real polling pattern**: every register write in this function is preceded by a
real "wait for GPU idle" loop reading `RBBM_STATUS` (`0xe40-0xe43`, byte-reassembled - re-confirming,
independently, the already-known register from a completely different function) combined with two
additional status bits at byte `0x1722` (not found in local docs - likely a display/CRT-controller
status register in a doc this project doesn't have) - real, textbook "don't touch pipe config while
the GPU might be mid-operation" synchronization.

**Not found in either local doc** (flagged honestly, not guessed): `0x15e0`-`0x15fc` (see below - now
understood functionally even without a name), `0x16e8`, `0x16cc`, `0x2284`'s neighbors `0x4614`/
`0x47c8`/`0x4bec`/`0x4398`, `0x70`/`0x74`/`0xf8-0xfb`/`0x130-0x134`/`0x6110`/`0x6910` (low addresses,
plausibly Memory-Controller/config-space registers outside this doc's 3D-acceleration focus), `0x774`/
`0x770`/`0x1fa8`, `0x170c`/`0x4124`/`0x4be8`, and the byte-level sub-addresses `0x6104`/`0x6904`/
`0x6148`/`0x6948`/`0x60c4`/`0x68c4` (read, not written, in `initialize_hardware` - captured into
per-device fields `this+0xb78..0xb8c`, likely real chip-revision/capability-strap readback).

**Real functional confirmation of `0x15e0`/`0x15e4` (already flagged as unresolved in the submit_buffer
entry above)**: `setup_R500_internal_space` explicitly zeroes the whole `0x15e0-0x15fc` range at
hardware-init time, and `initialize_hardware` writes the context's buffer-submission counter
(`this+0x50`) to `0x15e0` and the retirement counter (`this+0x850`) to `0x15e4` immediately after
`start_promo4_engine` succeeds. **This confirms the hypothesis from the submit_buffer entry above
without needing hardware**: `0x15e0`/`0x15e4` really are the live submission/retirement timestamp
scratch registers - initialized to the current (post-engine-start) counter values, then advanced by
`submit_buffer_retired`'s later writes. This is almost certainly the literal hardware backing for the
fence mechanism's `AGLContext+0x1838` live completion counter already found client-side.

Committed as this sweep continues - more sections below as further functions are decoded.
