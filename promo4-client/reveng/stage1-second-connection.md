# Stage 1: real second IOKit connection, alongside a live GL context

Tests the core hypothesis behind the whole PROMO4 direction: that a second, independent, hand-built
IOKit connection can coexist with Apple's own live GL driver connection to the same kext, without
disrupting either side. Read-only per Stage 1's own scope - no memory mapping, no command submission,
only the confirmed-safe selector 3 query call from Stage 0.

## Real IOKit service identity (new finding this stage)

`ioreg` on the real, running G5 confirms the actual registered service class:

```
+-o ATIRadeonX1000  <class ATIRadeonX1000, ...>
    "IOProviderClass" = "IOPCIDevice"
    "IOGLBundleName" = "ATIRadeonX1000GLDriver"
    "CFBundleIdentifier" = "com.apple.ATIRadeonX1000"
```

`IOServiceMatching("ATIRadeonX1000")` + `IOServiceGetMatchingService` finds it directly - no separate
"user client" nub is registered in the IORegistry (standard IOKit pattern: the client interface comes
from `ATIRadeonX1000`'s own `newUserClient` method, not a distinct registered object).

## Programs built

- `src/hold_context.c` / `hold-context` - opens one real AGL context (identical pattern to
  `finish_probe.c`'s setup: `aglChoosePixelFormat` -> `aglCreateContext` -> `aglCreatePBuffer` ->
  `aglSetCurrentContext`), confirms it's genuinely active with one draw+`glFinish()`, then holds it
  open for N seconds with a per-second heartbeat draw+finish (so it's provably live, not idle), then
  tears down cleanly.
- `src/stage1_probe.c` / `stage1-probe` - bypasses OpenGL/AGL entirely. Direct
  `IOServiceGetMatchingService("ATIRadeonX1000")` -> `IOServiceOpen(service, mach_task_self(), 1,
  &conn)` -> `IOConnectMethodScalarIScalarO(conn, 3, 0, 3, &o0,&o1,&o2)` -> `IOServiceClose(conn)`.
  Exactly Stage 0's confirmed-safe `_gldGetRendererInfo` sequence, replicated directly against the
  kext instead of through Apple's driver.

## Real results

**Baseline (`stage1-probe` alone, no live context)**:
```
IOServiceOpen OK, connect=0xf0b
selector 3 OK: out=(0x810040, 0xf7c8000, 0x10000000)
IOServiceClose: 0x0
```

**Concurrent test**: launched `hold-context 15` in the background, then ran `stage1-probe` three
times (roughly t=2s, t=5s, t=8s into the 15-second window) while it was actively looping
draw+`glFinish()` heartbeats once per second.

All three probe runs succeeded, identically to the baseline:
```
[stage1-probe:concurrent1] IOServiceOpen OK, connect=0xf0b
[stage1-probe:concurrent1] selector 3 OK: out=(0x810040, 0xf7c8000, 0x10000000)
[stage1-probe:concurrent1] IOServiceClose: 0x0
[stage1-probe:concurrent2] ... identical ...
[stage1-probe:concurrent3] ... identical ...
```

`hold-context`'s own log shows all 15 heartbeats completed with no gap, no GL error, no slowdown, and
a clean exit - fully undisturbed by the three independent connections opened and closed during its
lifetime.

## Verdict

**Confirmed, with real hardware evidence**: a second, independent, hand-built IOKit connection to
`ATIRadeonX1000` coexists cleanly with Apple's own live GL driver connection. Output values are
identical whether or not a live context is present, and identical across repeated runs - selector 3
is exactly the safe, side-effect-free query call Stage 0's static analysis predicted. No instability,
no SSH disruption, no rendering corruption observed in either side.

This directly de-risks Stage 2 (first real PM4 submission) on the connection-management question -
that part of the architecture is now empirically proven, not just architecturally plausible. Stage 2's
real remaining risk is entirely in the command-buffer/submission mechanism itself (memory type 1's
implicit-submit-on-remap behavior flagged as unconfirmed in Stage 0), not in opening a second
connection per se.

## Next: Stage 2 prep

Per Stage 0's own flagged open item, decode the real command-buffer header constants (`0x5c8`,
`0x20000`) written by `FUN_0001a0f0` before attempting any real submission - a minimal real PM4
packet should start with the safest possible payload (a PM4 NOP/no-op packet, per AMD's own R5xx
documentation) into a command buffer built with a structure Stage 0/1 haven't yet fully confirmed.
