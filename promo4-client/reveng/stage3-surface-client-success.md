# Stage 3: real success - the documented IOAccelerator surface client works

Direct continuation of `stage3-real-ioaccelerator-api.md`. Built `stage3d-surface-client`, opening a
connection with the real, documented `kIOAccelSurfaceClientType` (0) instead of the `type=1`
(`IOATIR500GLContext`) this whole project has used until now.

## Real, decisive, successful result

```
IOServiceOpen(type=0, kIOAccelSurfaceClientType) -> 0x0, connect=0xf0b
kIOAccelSurfaceGetState via scalarO: 0x0 out=0x1
kIOAccelSurfaceGetState via structureI(0): 0xe00002c2
health check (type=1, selector 3): 0x0 out=(0x810040,0xf7c8000,0x10000000)
```

- **`IOServiceOpen` with `type=0` succeeds** - confirms `ATIRadeonX1000` really does register the real,
  documented `kIOAccelSurfaceClientType` user client, exactly as `stage3-real-ioaccelerator-api.md`
  hypothesized from the shipped headers alone.
- **`kIOAccelSurfaceGetState` (selector 2) succeeds via the `scalarI_scalarO` shape** (0 in, 1 out),
  returning `0x1`. This **exactly matches the documented `kIOAccelSurfaceStateIdleBit` (`0x00000001`)**
  from `IOAccelSurfaceConnect.h`'s `eIOAccelSurfaceStateBits` enum - a real, meaningful, correctly-
  interpreted result, not just "a call that didn't error." This is the first call in the whole Stage 3
  thread whose *return value* is independently confirmed correct against real Apple documentation, not
  just "didn't crash."
- The `structureI` variant fails as expected (wrong shape for this selector) - useful negative
  confirming `scalarI_scalarO` is the real shape for this one.
- The original `type=1` GL context connection's own health check (selector 3) still matches its
  established baseline exactly - opening and using a `type=0` connection alongside has zero effect on
  the existing, already-proven connection type.

## Real significance

This is a genuine, clean, verifiable success using the real, public, documented API path identified in
the previous entry - not a guess that happened to not error, but a call whose result independently
matches Apple's own documented bit definitions. It directly confirms the core hypothesis: the real
render-target/surface management this whole Stage 3 thread has been trying to reach is reachable
through this real, separate, documented client type, and it behaves exactly as documented on this real
hardware.

## Real next step

Try the real, documented state-changing operations next - `kIOAccelSurfaceSetShape` (9) is the most
direct candidate for actually establishing a real, usable surface, followed by `kIOAccelSurfaceFlush`
(10). These are real state changes (unlike the read-only `GetState` query just confirmed), so still
warrant the same care as every other state-changing step in this project - reasoned parameters from the
real, documented structures (`IOAccelBounds`, `IOAccelSurfaceInformation`) rather than blind guesses,
and real health checks before/after.
