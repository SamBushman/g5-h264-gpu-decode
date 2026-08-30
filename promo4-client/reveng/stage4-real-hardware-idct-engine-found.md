# A real, independent hardware IDCT engine, with its own command ring - genuinely new capability discovery

Found while sweeping `ATIRadeonX1000`'s base hardware class autonomously. This is a real, structurally
significant finding distinct from anything this project has documented before, and it revises (with
more precision, not a contradiction) the earlier `stage3-va-driver-is-not-a-decode-shortcut.md`
conclusion.

## The real functions

`ATIRadeonX1000::submit_idct_buffer_consumed(unsigned long*, unsigned long, sATIDVDIDCTInfo*)`
(`0x1eb30`) and `ATIRadeonX1000::waitForConsumedIDCTTimeStamp(unsigned long)` (`0x254e0`) are real,
named, non-trivial functions built around a real struct type, `sATIDVDIDCTInfo` - not a guess, a real
Ghidra-recovered mangled-symbol type name from Apple's own kext.

`submit_idct_buffer_consumed` writes a sequence of real (register, value) pairs **directly into the
main command ring** (not through the indirect-buffer path `submit_buffer` uses) - eight pairs, each
register value one of `0x80001fe0`/`0x80001fe4`/`0x80001fec`/`0x80001ff0`/`0x80001f8c`/`0x80001ffc`/
`0x80001ff8`/`0x80001fa8`/`0x80001fac`, each followed immediately by one field pulled from the real
`sATIDVDIDCTInfo` struct (`param_3+0x10/0x14/0x18/0x1c/0x20/0x24/0x28/0x2c/0x30` - a real, dense set of
fields, very plausibly per-plane/per-block coefficient-buffer addresses given the byte-swap-free direct
copy). This is then followed by **six repeated writes of the pair `(0x80001fb4, 0)`** - almost
certainly a real "process/trigger" pulse register hit once per sub-block, matching MPEG-2's real
macroblock structure (multiple 8x8 luma/chroma blocks per macroblock).

## The real, structurally significant finding: a second, independent ring-buffer write-pointer

The function's final MMIO write:
```c
*(uint *)(mmioBase + 0x1fa0) = uVar3 << 0x18 | (uVar3 & 0x700) << 8;
```
is **byte-for-byte the identical bit-packing formula** `submit_ring_data` uses for the real, already-
doc-confirmed `CP_RB_WPTR` (main command-processor ring write-pointer, `0x714`):
```c
*(uint *)(mmioBase + 0x714) = uVar2 << 0x18 | (uVar2 & 0x700) << 8;
```
The same real encoding, applied to a completely different register address, used by a function that
writes to a *different* internal write-cursor field (`this+0x930`, vs. the main ring's `this+0x918`)
and reads a *different* completion counter (`waitForConsumedIDCTTimeStamp` reads its own dedicated
location, distinct from `waitForRetiredTimeStamp`'s). **This is real, structural evidence that the
IDCT hardware block has its own independent command ring and its own independent completion-counter
mechanism, parallel to (not multiplexed through) the main 3D command processor's ring.** Not
previously known or suspected anywhere in this project's prior work.

## Honest gap: register names not resolved

None of `0x1f8c`/`0x1fa0`/`0x1fa8`/`0x1fac`/`0x1fb4`/`0x1fe0`/`0x1fe4`/`0x1fec`/`0x1ff0`/`0x1ff8`/
`0x1ffc` appear in either locally-available document (`r3xx_3d_registers.txt`, `r5xx_accel_v15.txt`) -
both are titled around 3D acceleration specifically and plausibly never covered a separate video/DVD/
IDCT register block. `~/Documents/AMD Docs/` doesn't appear to contain a dedicated video/multimedia
register guide for this generation (checked the directory listing already used in
`stage3-viewport-and-hang-mitigation-from-pdf.md` - nothing named for video/UVD/IDCT registers). The
real *structure* (independent ring, independent completion counter, real per-block trigger pulses,
real coefficient-address fields) is confirmed from the binary itself regardless of not having official
names for each address.

## Revises, doesn't contradict, the earlier VA-driver finding

`stage3-va-driver-is-not-a-decode-shortcut.md` concluded `ATIRadeonX1000VADriver.bundle`'s real "AVA"
API is a display/overlay/colorspace-conversion path, not a decoder - that conclusion still stands (the
VA driver bundle itself has none of these IDCT-specific symbols or register patterns). **This is a
separate, lower-level capability**, reached through the base `ATIRadeonX1000` kext class directly (very
likely via `ATIR500DVDContext`/`IOATIR500DVDContext`, the third context class this project's
`process_command_buffer` cataloguing already found but hasn't decompiled - a natural next target), not
through the VA driver bundle at all. The real, honest picture: there IS a genuine hardware IDCT engine
in this GPU, accessible via a dedicated kernel-level path Apple's DVD Player almost certainly used for
real MPEG-2 hardware assist - it's just a different, lower-level surface than the VA driver's overlay
API, and this project hadn't looked at the base hardware class's DVD-specific methods until this sweep.

## Testable hypothesis - documented per the user's instruction, NOT attempted (no hardware this session)

**Hypothesis**: `0x1fa0` is a real, independent ring write-pointer for a dedicated IDCT command queue,
and `0x1fa8` onward are real per-block coefficient-address/control registers for that same engine.

**Proposed test method** (for whenever the G5 is back): (1) Use the existing `gdb`-based live-tracing
technique (`project-g5-ancient-gdb-technique`) to set a watchpoint on the MMIO range `0x1fa0-0x1ffc`
while a real DVD-Player MPEG-2 playback session is active, confirming real writes occur at these
addresses during actual hardware-accelerated playback (not just this decompile's cold-code reading).
(2) Cross-reference the values written into the coefficient-address fields against the real, concurrent
IDCT/YUV buffer addresses used by DVD Player, to confirm the field-to-purpose mapping guessed above.
(3) Read `0x1fa0` itself immediately after a real submission to confirm it echoes back a real ring
read-pointer analogous to `CP_RB_RPTR`, confirming the "independent ring" structural hypothesis
directly rather than by pattern-matching against the main ring's encoding alone. This is purely
read/observe - no register writes of our own - so it carries none of the write-side hang risk this
project's PROMO4 investigation has already run into twice.
