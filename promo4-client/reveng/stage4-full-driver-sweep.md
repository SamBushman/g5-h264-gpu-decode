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

## `load_promo4_micro_code`/`start_promo4_engine`/`stop_promo4_engine` - real, complete, register-confirmed lifecycle

Decompiled all three (`0x1cd30`/`0x1cd90`/`0x1cb00`). **`load_promo4_micro_code`** is exactly what its
name says: writes `CP_ME_RAM_ADDR` (`0x7d4`, reset to 0 = start address), then loops 256 times writing
one dword each to `CP_ME_RAM_DATAH`/`CP_ME_RAM_DATAL` (`0x7dc`/`0x7e0`) from a local 2KB data blob
(`&DAT_0004ca60`) - **this is the literal embedded microcode blob** an earlier session's kext decompile
already inferred exists ("reuse Apple's own microcode blob, don't author one") - now seen loading via
the real, exact, doc-confirmed register pair.

**`start_promo4_engine`** real register sequence, every one confirmed: `CP_RB_CNTL` (`0x704`,
configured with ring-size/fetch flags), `CP_RB_WPTR_DELAY` (`0x718`), `CP_RB_WPTR` (`0x714`, reset),
`CP_RB_RPTR_ADDR` (`0x70c`), `CP_CSQ_CNTL` (`0x740`, set to `0x40`) - then a real polling loop on
`RBBM_STATUS` bits (`0xe41`/`0xe42`, mask `0x10200`) waiting for the CP to report ready, with a bounded
1,000,000-iteration timeout before giving up. A complete, real "bring the ring buffer online" sequence.

**`stop_promo4_engine`** is the exact inverse, also fully register-confirmed: a bounded busy-wait on
`RBBM_STATUS`+the same `0x1722` status byte from `setupR520Pipes`, then `0x770`→0 (shutdown flag, not
found by name in local docs), `CP_RB_CNTL` reconfigured to a disabled state, `CP_CSQ_MODE`→0,
`CP_RB_RPTR_WR`→0, `CP_RB_WPTR`→0 - genuinely tearing down what `start_promo4_engine` brought up,
register for register.

## `waitForRetiredTimeStamp`/`waitForTimeStampNoLock` - the real fence completion mechanism, confirmed as DMA'd system memory, not a register

Decompiled both (`0x25080`/`0x25360`) - real implementations of the timestamp-wait family
`stage3-fence-mechanism.md` speculated about from the client side. **Confirms the completion counter is
NOT a simple MMIO register** - it's read via:
```c
pbVar1 = (byte *)(*(int *)(this + 0x864) + *(int *)(this + 0x868));  // base + offset, a real pointer
uVar2 = pbVar1[3]<<24 | pbVar1[2]<<16 | pbVar1[1]<<8 | pbVar1[0];      // same LE hardware byte-swap pattern
```
`this+0x864` is a base pointer to a real shared memory region (almost certainly the GPU-DMA'd scratch
area this project already knows exists, given the identical byte-swap-from-hardware pattern seen
everywhere else), with different fixed offsets (`+0x868` here, `+0x86c` in the `NoLock` variant, and
presumably a third for the IDCT-specific family below) selecting which of several counters to read -
consistent with genuinely independent submission/retirement/IDCT-consumption completion tracking. Real
bounded wait loop (up to `0xc351` = 50001 retries, ~100us sleep each via `FUN_000251b4`/a real
Mach `assert_wait`-style primitive) before giving up. This is almost certainly the real, ultimate
hardware backing for both the client-side fence mechanism's `AGLContext+0x1838` counter AND the
`0x15e0`/`0x15e4` scratch-register hypothesis above - worth reconciling if this sweep reaches the
function that actually sets up `this+0x864/0x868/0x86c`'s real addresses (not yet found).

## `ATIR500DVDContext`/`ATIR500Surface` overlay path - real structure found, register writes not reached this pass

Decompiled the real DVD-context overlay chain: `ATIR500DVDContext::write_regs`/`dvd_setup_overlay`/
`dvd_enable_overlay`/`dvd_enable_deint` all delegate to `ATIR500Surface::dvd_setup_overlay`/
`enable_overlay`/`disable_overlay`/`enable_deint`/`showbuffer`. **Honest result**: at this call depth,
these are all real but currently trivial - they store parameters into per-surface state fields
(`this+0x94/0x96/0x98/0x9a` for overlay geometry, `this+0xdac` for deinterlace mode) or are outright
no-ops in this build (`enable_overlay`/`disable_overlay`/`showbuffer` all have empty bodies). The real
MMIO overlay-scaler register writes (this GPU's real `OV0_*`-family registers, by analogy with the
public ATI/Radeon overlay architecture, though not confirmed by name here) are not reached at this
depth - most likely applied later, either from a vsync/interrupt-driven "commit staged overlay state"
handler not yet located, or from a code path this exact kext build/config doesn't exercise. `write_regs`
is architecturally notable on its own: a generic, always-present passthrough that writes an arbitrary
caller-supplied (register, value) pair, masked to a 0x1FFC-byte window (`this_dvd_context+0x8c`'s MMIO
base) - i.e. a real, intentional "let userspace poke a bounded slice of MMIO space" escape hatch,
presumably how DVD Player itself configures overlay hardware without needing dedicated kernel API for
every register.

## Follow-up top-down pass: `write_r500_3d_blit_state_packet` and `GetTextureOffset` newly tractable

Prompted by the user's question ("does this research enable top-down analysis you couldn't do
before?") - yes: revisited `ATIR500GLContext::write_r500_3d_blit_state_packet` (`0x2ac10`), the
function `stage3-scope-assessment.md` originally flagged as too complex to fully decode. With the
register map and HyperZ logic already confirmed, it decodes cleanly this time (only 116 lines - far
smaller than it looked when opaque).

**Real structural finding**: it writes into a real, named staging struct
(`r500_3d_blit_state_packet_struct*`), not directly into a raw PM4 array - a different, complementary
convention from `write_kernel_context_buffer_regs`'s flat (index,value) array. This struct is very
likely serialized into real PM4 content by a separate function not yet located - a concrete next
target if this thread continues.

**Fourth independent confirmation of `SC_CLIP_RULE = 0xaaaa`** (`param_1+0x2b0 = 0xaaaa`, verbatim) -
now confirmed from: AMD's own docs' bit-field description, the KolibriOS reference driver, the GA
plugin's `write_kernel_context_buffer_regs`, and this function - four independent sources agreeing on
one exact value.

**Real progress on the previously-undecoded 40-entry format table** (`DAT_0004d2dc`/`DAT_0004d2e0`,
28-byte stride, dumped but never interpreted since `stage3-embedded-opcode-language.md`): this function
extracts five separate bit-fields from one table entry (table bits `[17:21]`, `[15:16]`, `[3:4]`,
`[11:12]`, `[9:10]` - each independently masked and repositioned) and combines them into a single
output field (`param_1+0x228`, very likely a real tiling/pitch-control register given the shape).
This is real, partial progress on a genuinely hard problem this project flagged as unsolved months ago
- full resolution would need locating the struct-to-PM4 serializer to confirm which real register
`+0x228` becomes, not completed this pass.

**`GetTextureOffset`** (`0x280c0`) decoded: real per-texture-backing-type offset computation
(`VendorTextureBuffer+0x20`'s type byte selects among VRAM-direct, GART-mapped, and surface-backed
paths), and the GART-mapped path reuses the exact same GART aperture base field (`this+0x8a4`) already
confirmed in `submit_buffer` - another small, real cross-function confirmation.

Committed as this sweep continues - more sections below as further functions are decoded.
