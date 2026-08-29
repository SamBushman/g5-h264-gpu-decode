# Stage 2a: full real context memory allocation from a hand-built client

The first genuinely new real-hardware-risk step (real GART-backed GPU memory allocation, not just a
read-only query) - but deliberately scoped to stay within call shapes already fully decompiled and
confirmed safe (Stage 0/1/2-prep), stopping short of writing payload content or triggering a
buffer-consumption cycle, which remains open pending resolution of the raw-PM4-vs-intermediate-format
question flagged in `stage2-prep-command-buffer.md`.

## What it does

`stage2a-context-alloc` replicates `_gldCreateContext`'s exact real decompiled sequence (Stage 0),
byte-for-byte, through a hand-built client (no AGL/CGL at all, same approach as Stage 1's probe):

```
IOServiceOpen(service="ATIRadeonX1000", type=1, &conn)
selector 3 (scalarI_scalarO, 0 in / 3 out)
IOConnectMapMemory(conn, 0, task, &addr0, &size0, options=0x101)
IOConnectMapMemory(conn, 1, task, &addr1, &size1, options=1)
IOConnectMapMemory(conn, 2, task, &addr2, &size2, options=1)
IOConnectMapMemory(conn, 4, task, &addr4, &size4, options=1)
```
Then unmaps all four and closes cleanly. No payload is ever written into any of the mapped regions;
no re-map/consumption cycle is triggered.

## Real results (standalone, then concurrent with a live GL context)

Both runs produced **identical** output:
```
selector 3 OK: out=(0x810040, 0xf7c8000, 0x10000000)
IOConnectMapMemory(type=0) OK: addr=0x5000 size=0x1000
IOConnectMapMemory(type=1) OK: addr=0x6000 size=0x20000
IOConnectMapMemory(type=2) OK: addr=0x26000 size=0x8000
IOConnectMapMemory(type=4) OK: addr=0x2e000 size=0x1000
```

**Real, concrete confirmations from live hardware data**:
- **Memory type 0 is exactly 0x1000 (4096) bytes** - an exact match to `stage2-prep-command-buffer.md`'s
  static finding that the kernel's `clientMemoryForType` hardcodes `*param_2 = 0x1000` for type 0.
  Confirms the status/register-page hypothesis with live data, not just decompiled code.
- **Memory type 1 is exactly 0x20000 (131072) bytes** - this resolves a real open question from Stage 0/
  2-prep: the literal constant `0x20000` written by the GL driver's own `FUN_0001a0f0` into its
  buffer-tail record is **not** mysterious PM4/opaque-format content - it is the real, literal size (in
  bytes) of the command buffer that was just mapped. The client is recording its own buffer's size as
  part of a bookkeeping record, not encoding a hardware instruction. This meaningfully de-risks the
  "what does the payload area actually contain" question - at least this one specific field is now
  understood with confidence.
- **Memory type 2 is 0x8000 (32768) bytes** and **memory type 4 is 0x1000 (4096) bytes** - new real
  data points; purpose of type 2 remains undetermined, type 4's small size is consistent with a fence/
  status region (matches Stage 0's `_gldCreateFence` finding).

**Concurrent test**: ran `hold-context` for 15s in the background, `stage2a-context-alloc` at t=2s.
Output identical to the standalone run - all four mappings succeeded, cleanup ran, connection closed.
`hold-context`'s own heartbeat log shows all 15 heartbeats completed with no gap, no GL error, clean
exit - completely undisturbed, exactly matching Stage 1's result but now with a much heavier real
operation (actual GPU memory allocation, not just a query) running concurrently.

## One real, honest wrinkle - not a stop condition

Unmapping memory type 4 returned `0xe00002c2` (`kIOReturnBadArgument`) in both runs, consistently.
Types 0, 1, and 2 all unmapped cleanly (`0x0`). Connection close still succeeded regardless
(`IOServiceClose: 0x0`) - no leak, no instability, no hang. Not yet root-caused (possible causes:
`IOConnectUnmapMemory`'s call shape may differ for this specific memory type on this kext, or
unmap-ordering matters and type 4 - mapped last - needs to unmap in a different relative position).
Real, reproducible, minor - flagged as an open item, not investigated further this pass since it
doesn't block forward progress and the connection still tears down cleanly either way.

## Verdict

A hand-built client can perform a complete, real, independent GL-context-shaped memory allocation
(all four real memory types, genuine GART-backed GPU resources) alongside Apple's own live driver
connection, with zero disruption to either side and results indistinguishable from the standalone
case. This is real, meaningful de-risking of Stage 2's allocation/multi-client-safety question,
achieved without yet crossing into the genuinely unresolved payload-content question. That remains the
real next step: resolving whether the payload area (bytes at buffer offset 0x20 onward, per the
`VendorCommandBufferHeader`'s own capacity field) expects raw PM4 or an Apple-intermediate format,
before writing anything into it and triggering a real consumption cycle.
