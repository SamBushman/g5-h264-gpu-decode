# Stage 0: external-method dispatch table (real findings)

Static Ghidra analysis of `ATIRadeonX1000GLDriver.bundle` (headless, `PowerPC:BE:32:default` +
`macosx` cspec, same project/method as every other decompile this project has done). Finds and
decompiles every real caller of the IOKit method-call family this bundle imports, extracting the
actual selector numbers, memory-type IDs, and calling conventions Apple's own GL driver uses against
this exact kext - the gap flagged in `promo4-client-protocol.html` §3.

No hardware touched. Pure static analysis, matching Stage 0's own zero-risk scope.

## Confirmed imports (this binary's real external-method surface)

Only **three** of Apple's four documented `io_connect_method_*` shapes are actually imported by this
bundle - `scalarI_scalarO`, `structureI_structureO`, `scalarI_structureI`. `scalarI_structureO` is
never used here. Also imports `IOConnectMapMemory`, `IOServiceOpen`, `IOServiceClose`,
`IOConnectAddClient` (the last one wasn't in this project's earlier decompile of `_gldCreateContext` -
a real, previously-missed detail, called conditionally on `param_4`).

## Real, confirmed values

| Item | Value | Confirmed by |
|---|---|---|
| User-client type (3rd arg to `IOServiceOpen`) | **1** | `_gldCreateContext`, `_gldGetRendererInfo` - two independent call sites, same constant |
| Memory type 0 | mapped with `options=0x101` (differs from all others, which use `1`) - status/register region is the leading guess given the distinct flag | `_gldCreateContext` |
| Memory type 1 | **command buffer pool** - mapped once at context creation, then re-mapped repeatedly (a fresh chunk each time) whenever the current buffer runs low | `_gldCreateContext` (initial map) + `FUN_0001a0f0` (repeated re-map, called from both `_gldFlush` and `_gldFinish`) |
| Memory type 2 | purpose not yet determined - six output fields zeroed before the call, more complex than types 0/1/4 | `_gldCreateContext` only so far |
| Memory type 4 | **fence/synchronization region** - a CPU-side bitmap (`malloc(size>>5)`) tracks free slots directly against this region's own layout | `_gldCreateContext` (initial map) + `_gldCreateFence` (re-map + bit-allocator) |
| Selector 3, via `scalarI_scalarO` | **init/query call** - 0 scalar inputs, 3 scalar outputs. Works on a *minimal* connection (`IOServiceOpen` → selector 3 → `IOServiceClose`, no full context setup) as well as inside full context creation | `_gldCreateContext` (full context path) + `_gldGetRendererInfo` (minimal, standalone path - real, already-safe precedent for Stage 1) |
| Selector 8, via `scalarI_structureI` | **finish/wait call** - 0 scalar inputs, 0-byte structure input. Called in a real retry loop, `while (result == -0x1ffffd2a)` (0xE00002D6 unsigned - a specific busy/not-ready `IOReturn` code, exact symbolic name not yet verified) | `_gldFinish` |

## Real call sequences, verbatim (decompiled, not reconstructed)

**Context creation** (`_gldCreateContext`, abbreviated to the protocol-relevant calls):
```
IOServiceOpen(service, userClientType=1, &connection)
IOConnectAddClient(connection, ...)                          // conditional on param_4
io_connect_method_scalarI_scalarO(connection, 3, 0,0, &out1, outCount=3)
IOConnectMapMemory(connection, 0, task, &addr0, &size0, options=0x101)
IOConnectMapMemory(connection, 1, task, &addr1, &size1, options=1)
IOConnectMapMemory(connection, 2, task, &addr2, &size2, options=1)
IOConnectMapMemory(connection, 4, task, &addr4, &size4, options=1)
```

**Minimal renderer-info query** (`_gldGetRendererInfo`, real, already-proven-safe, no full context
needed - the direct Stage-1 candidate):
```
IOServiceOpen(service, userClientType=1, &connection)
io_connect_method_scalarI_scalarO(connection, 3, 0,0, &out1, outCount=3)
IOServiceClose(connection)
```

**Submit / flush** (`FUN_0001a0f0`, called from both `_gldFlush` and conditionally from
`_gldFinish` when the current command buffer is nearly full):
```
// writes a real command-buffer header (const values 0x5c8, 0x20000 - not yet decoded further)
// at the current write cursor, then:
IOConnectMapMemory(connection, 1, task, &newBufAddr, &newBufSize, options=1)
// ... updates internal write-cursor/capacity bookkeeping for the new chunk
```
Real, useful detail: command submission on this driver is **not** a single "submit this buffer"
external-method call - it's implicit in re-mapping memory type 1. The kext appears to consume/execute
whatever was written into the *previous* mapping as a side effect of the client asking for a new one.
This is a real, concrete protocol shape `promo4-client-protocol.html` §4 didn't anticipate (it assumed
a dedicated submission selector) - worth updating the proposal to reflect this once confirmed further.

**Wait / finish** (`_gldFinish`):
```
do {
    result = io_connect_method_scalarI_structureI(connection, 8, 0,0,0,0);
} while (result == -0x1ffffd2a /* 0xE00002D6 */);
```

## Open items for Stage 0's continuation

- Decode the real command-buffer header constants (`0x5c8`, `0x20000`) written by `FUN_0001a0f0` -
  likely a size/flags pair for the new chunk, would confirm or refute the "re-map = implicit submit"
  reading above.
- Memory type 2's real purpose - not yet touched by any decompiled function.
- The exact meaning of selector 3's three individual output scalars (only the first, `out1`, has a
  known consumer so far - passed into `FUN_0001b150`/stored into `puVar6[9]`, not yet decompiled).
- Confirm `0xE00002D6`'s real symbolic `IOReturn` meaning (`kIOReturnNotReady` is the leading guess by
  value shape, not yet cross-checked against a real header).
- Decompile the kext's own side (`ATIRadeonX1000.kext`, already in this project's Ghidra scratchpad
  from earlier sessions) to confirm what selectors 3 and 8 actually *do* kernel-side, not just how
  they're called from userspace.

## Recommendation for Stage 1

Replicate `_gldGetRendererInfo`'s exact minimal sequence - `IOServiceOpen` (type 1) → selector 3 via
`scalarI_scalarO` → `IOServiceClose` - alongside a live GL context (e.g. `finish-probe` or
`gpu-live-decode-test` running normally). This is not a novel operation being tried for the first
time - it's the same round trip Apple's own driver performs every time an application queries GL
renderer info, which happens constantly and safely today. If this coexists cleanly, Stage 1 is
answered with about as much real precedent behind it as this project can get before touching hardware
in a genuinely new way.
