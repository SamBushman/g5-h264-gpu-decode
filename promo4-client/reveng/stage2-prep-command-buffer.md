# Stage 2 prep: real command-buffer header layout, kernel-side lifecycle, and PM4 packet formats

Continuing static RE (zero hardware risk) before attempting Stage 2's real submission. This time on
the KEXT side (`ATIRadeonX1000.kext`, project already in this session's Ghidra scratchpad from earlier
work), not just the GL driver bundle - directly answering Stage 0's own flagged open item ("decompile
the kext's own side to confirm what selectors 3/8 actually do kernel-side, not just how they're
called").

## Real class hierarchy found

The kext's real (unmangled, present in its own symbol table) class names: `IOATIR500Accelerator`
(the top-level service, matches the `ATIRadeonX1000` IOClass seen in `ioreg`), and per-client-type
context classes `IOATIR500GLContext`, `IOATIR5002DContext`, `IOATIR500DVDContext`,
`IOATIR500Surface`, `IOATIR500Shared` - each with a matching non-`IO`-prefixed pure-logic class
(`ATIR500GLContext` etc.) implementing `getTargetAndMethodForIndex`. This confirms userClientType=1
(Stage 0) maps to `IOATIR500GLContext`/`ATIR500GLContext` specifically - the GL-context user client,
exactly matching what `_gldCreateContext` opens.

## Real selector dispatch mechanism confirmed

`ATIR500GLContext::getTargetAndMethodForIndex` (real decompile):
```c
int ATIR500GLContext::getTargetAndMethodForIndex(ATIR500GLContext *this, IOService **param_1, ulong param_2) {
  *param_1 = (IOService *)this;
  if (param_2 < 0x14) {              // selectors 0-19: a real per-instance IOExternalMethod array
    return param_2 * 0x18 + *(int *)(this + 0x2a0);   // 0x18 = 24 bytes/entry, standard Tiger-era size
  }
  if (param_2 != 0x14) { return 0; } // selector 20: one extra, separately-handled special case
  return *(int *)(this + 0x360);
}
```
Confirms selectors 3 and 8 (Stage 0) are real entries in a genuine 20-entry classic IOKit
`IOExternalMethod` table, populated per-instance at offset `this+0x2a0` (contents not yet traced to
their initializer - would need to find the constructor/init call that fills this array to get the
real kernel-side function name/argument-count for each selector directly, not yet done this pass).

## Real memory-type dispatch confirmed, with a genuine surprise

`IOATIR500GLContext::clientMemoryForType` (per-instance dispatcher, called on every
`IOConnectMapMemory`):
- **Type 0**: returns a real, always-present memory descriptor for a **fixed, small 4096-byte
  (`0x1000`) region** - `*param_2 = 0x1000` literally in the decompile. This is a strong, concrete
  confirmation of Stage 0's guess ("status/register region") - 4KB is exactly the right size for a
  hardware status/doorbell page, far too small to be a command or texture buffer.
- **Type != 0** (including type 1, the command buffer): dispatches through a shared handler pointed
  to by `PTR_clientMemoryForType_00047558`, which resolves to `IOATIR500GLContext::clientMemoryForType`
  itself (i.e. the real switch lives in one function, keyed on `param_1`).

**The real surprise, found in the type-1 case's actual code**: this is NOT a simple "allocate and
return a buffer" call. It walks a real linked list (`this+0xe8`/`this+0xec`, iterating via a `+0x3c`
next-pointer field) moving entries into a separate free/retired list (`this+200 -> +0x5cc/+0x5d0`,
with a count at `+0x5d4`), THEN calls `init_command_buffer_header` on a freshly-selected buffer before
handing it back. This is real, concrete, kernel-side evidence for Stage 0's "re-map = implicit submit"
hypothesis: **the act of calling `IOConnectMapMemory(conn, 1, ...)` again doesn't just allocate a new
buffer, it retires the previous one through what looks like a real completion/consumption
bookkeeping path**, exactly matching how a real command-processor client driver is expected to behave
(hand off a filled buffer, get a fresh one, previous one becomes available again once consumed).

**Honestly unresolved**: whether this same call SYNCHRONOUSLY triggers the actual GPU submission (an
MMIO ring-pointer write) right here, or just marks it for a separate workloop/interrupt-driven
consumer to pick up later. A narrower follow-up scan (101 real functions across the driver's own
`ATIR500`/`Accelerator`/`Ring`/`CP`-named functions, searching for the ring write-pointer offset
`0x714` confirmed in an earlier session, or literal "submit"/"doorbell" text) found **zero direct
hits** - the actual MMIO trigger is hidden behind indirection this pass didn't chase down (a generic
register-write helper, most likely). Not resolved; flagged honestly rather than guessed past.

## Real `VendorCommandBufferHeader` layout (partial, but concrete)

`IOATIR500GLContext::init_command_buffer_header` (real decompile, called every time a fresh type-1
buffer is handed to the client):
```c
void IOATIR500GLContext::init_command_buffer_header(IOATIR500GLContext *this, VendorCommandBufferHeader *param_1, ulong param_2, ulong param_3) {
  // bytes 0x00-0x1F (8 dwords) zeroed
  *(ulong *)(param_1 + 0x14) = param_3;                    // caller-supplied tag/id
  *(undefined4 *)(param_1 + 0x20) = 0x1000000;              // fixed constant - SAME value _gldFinish
                                                             //   passes as FUN_0001a0f0's flush flag
  *(ulong *)(param_1 + 0x10) = param_2 - 0x20 >> 2;          // (bufferSize - 32) / 4 = payload capacity
                                                             //   in dwords - confirms header is 32
                                                             //   bytes, payload starts right after
  *(undefined4 *)(param_1 + 0x1c) = 1;                       // fixed version/flag field
  *(undefined4 *)(param_1 + 0x18) = *(undefined4 *)(this + 0x7c);  // per-context tag
}
```
The literal constant `0x1000000` appearing on BOTH sides independently (kernel's header-init AND the
GL driver bundle's own `_gldFinish` -> `FUN_0001a0f0(ctx, 0x1000000)` call, decompiled in Stage 0) is
a real, strong cross-confirmation that these two independently-decompiled code paths are talking about
the same protocol field - not a coincidence.

**Real, still-open**: what the client-written 16-byte record (marker pointer, 0, `0x5c8`, `0x20000`,
per Stage 0's `FUN_0001a0f0` decompile) written into the OUTGOING buffer's tail actually is. Checked
whether `0x5c8` decodes as a real PM4 Type-0 header per AMD's own R5xx spec (bits 31:30=TYPE,
12:0=BASE_INDEX, 29:16=COUNT): it would decode as TYPE=0, COUNT=0, BASE_INDEX=0x5c8 (register DWORD
offset 0x5c8, byte offset 0x1720 - not a register this project has independently confirmed the
identity of). Plausible but NOT confirmed - could equally be an Apple-internal chain-descriptor format
that the kernel translates before it ever reaches the real PM4 ring. Not resolved this pass.

## Real PM4 packet formats (AMD R5xx Acceleration doc v1.5, official, already local)

Confirmed via direct text extraction (`ps2ascii`, §6.1.3/6.1.4/6.2.1):

- **Type-2 packet**: the safest possible packet. ONE dword total (header only, no body). Bits 31:30 =
  `10` (TYPE=2), rest of the bits are reserved/don't-care. Explicitly documented as a pure filler the
  microengine silently skips - "used to fill up the trailing space... allows the microengine to skip
  the trailing space and fetch the next packet." A minimal, valid Type-2 packet is literally the single
  dword `0x80000000` (TYPE=2, everything else zero).
- **Type-3 packet, IT_OPCODE=0x10 (NOP)**: header is `TYPE=3` (bits 31:30), `COUNT`=N-1 (bits 29:16),
  `IT_OPCODE=0x10` (bits 15:8), reserved=0 (bits 7:0) - followed by N dwords of body, content ignored,
  explicitly "skip N DWORDs to get to the next packet."

Both are real, officially documented, and about as safe as a PM4 payload can be - if raw PM4 does turn
out to be what belongs in the payload area, either is a defensible minimal first packet.

## Decision for Stage 2's actual first hardware test

Given the genuine, honestly-unresolved ambiguity above (raw PM4 vs. an Apple-intermediate format in
the payload area; exact synchronous-vs-deferred submission trigger), inventing a novel hand-crafted
PM4 payload for the very first real hardware test would be testing an unconfirmed hypothesis on top of
an already-real risk. The lower-risk, still-genuinely-real design: **replicate the exact, already-fully-
decompiled real client sequence byte-for-byte** (`_gldCreateContext`'s real type 0/1/2/4 mapping calls,
then `_gldFinish`'s real chain-record-write + `0x1000000` marker + remap + selector-8-retry sequence)
from a hand-built client that never goes through AGL/CGL at all - i.e. prove an independent client can
perform one genuine, complete, real buffer-lifecycle round trip using literally the same bytes Apple's
own driver writes for an empty flush, rather than inventing new payload content. This tests the real
Stage 2 hypothesis (a hand-built client can drive the real submission path) without also gambling on
an unconfirmed packet-format guess in the same step.
