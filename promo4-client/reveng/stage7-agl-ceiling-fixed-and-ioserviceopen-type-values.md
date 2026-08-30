# AGL.framework's "decompile ceiling" was a false conclusion, and it directly resolved a real open gap

Per the user's explicit instruction to dig into prior tooling ceilings until resolved, not just document
them. Two of the three cited ceilings are corrected here; the third (`registerNotificationPort`'s bad
instruction data) is covered separately.

## The AGL.framework "ceiling" was project-state corruption, not a Ghidra/PowerPC limitation

Earlier this project concluded `AGL.framework` hit "a genuine Ghidra/PowerPC-prelinked-binary
compatibility limit" after repeated "Unable to resolve constructor" p-code errors inside the shared,
multi-binary `AGLProject`. **Re-importing the same exact binary (`ghidra-bins/AGL`) into a fresh,
dedicated, single-binary project decompiles it perfectly** - `_aglCreateContext`, `_aglSetCurrentContext`,
`_aglDestroyContext` all produced clean, real, high-quality decompiled C. The same fix (fresh, dedicated
per-binary project instead of a shared multi-import project) was already what unlocked the kext and
`libGLProgrammability.dylib` earlier this session - this generalizes that fix to a third binary and
retroactively identifies the true cause: **decompiling multiple large, unrelated Mach-O binaries inside
one shared Ghidra project produces cross-contaminated analysis state that manifests as p-code/constructor-
resolution failures on ones analyzed alongside others - not a true architecture/format limitation.**
This is a real, generalizable lesson for any future work in this project: always use one dedicated
project per binary, never a shared one, even when convenient.

## Real content recovered once decompilable: AGL is a thin wrapper over CGL

`_aglCreateContext` (`0x97c6adb4`) confirms AGL never talks to the kext or driver bundle directly - it
delegates everything to CGL (`_CGLCreateContext`, `_CGLDescribePixelFormat`, `_CGLSetParameter`,
`_CGLSetCurrentContext`, `_CGLDestroyContext`). It also reveals a real, previously-unknown struct field:
AGL's own opaque context wrapper (`malloc(0xafc)`, i.e. 2812 bytes) embeds a sub-structure at a fixed
`+0xaac` offset whose `+4` field holds the real `CGLContextObj` pointer (read by `__cvtToAGLCtx` in every
one of these functions). This is an AGL-wrapper-level offset, distinct from and not to be confused with
the already-documented driver/kernel-side `AGLContext+0x17dc`-family offsets used throughout this
project's live-injection work - two separate numbering systems for two different layers.

## `libGL.dylib`'s "+0x1c mystery" is confirmed to be a genuine live-state gap, not a ceiling

For completeness: the same fresh-import treatment was applied to `libGL.dylib` and `OpenGL.framework`.
`_glClear` decompiles identically to before - the dispatch slot in question (`puVar2[0xb]`) is populated
at runtime by whichever GLD-plugin renderer bundle is currently loaded (software or hardware), which is
information that genuinely does not exist in the static binary. **This one was correctly categorized
before** - the fresh-project fix resolves *decompile-quality* ceilings, not *missing-runtime-state* gaps,
and this project's earlier judgment correctly distinguished the two for this specific case.

## Real bonus: this directly resolved the open `IOServiceOpen` "type" value gap from `stage5-...md`

Searching the now-cleanly-decompilable `ATIRadeonX1000GLDriver.bundle` for `_IOServiceOpen` callers (a
search that could have been done earlier, but is far more useful now that the same fresh-project
discipline was applied everywhere) found two real, concrete call sites:

- **`_gldCreateContext`** (`0x6700`): `_IOServiceOpen(service, masterPort, **1**, &connection)` -
  **confirms `type=1` opens the GL context** (`ATIR500GLContext`/`IOATIR500GLContext`).
- **`_gldAttachDrawable`** (`0x70d0`, the `param_2==0x36` branch): `_IOServiceOpen(service, masterPort,
  **0**, &connection)` - **confirms `type=0` opens the Surface context** (`ATIR500Surface`/
  `IOATIR500Surface`).

Two more call sites (`FUN_00039100`, a throwaway capability probe, and `_gldGetRendererInfo`) both also
use `type=1`, consistent.

**A strong bonus validation**: every one of these real call sites' external-method invocations
(`_io_connect_method_scalarI_scalarO`/`_io_connect_method_scalarI_structureI`) matches the GL context's
external-method table decoded from raw bytes in `stage5-iouserclient-external-method-api-complete.md`
*exactly*, selector-for-selector and argument-count-for-argument-count: selector `3` (`get_config`)
called with output count `3`; selector `5` (`get_surface_size`) with output count `4`; selector `4`
(`get_status`) with output count `1`; selector `0xe` (`scale_surface`) with struct-input count `3`;
selector `0x10` (`set_surface_volatile_state`) with struct-input count `1`. This is a complete,
independent, real-code confirmation that the raw-table decode in `stage5` was correct in every detail
checked - not just plausible-looking, actually exercised by real Apple code exactly as decoded.

**Types `2` and `3` (2D and DVD contexts) remain unconfirmed by direct call-site evidence** - a real,
honest negative result, not an oversight: neither `ATIRadeonX1000GA.plugin` nor
`ATIRadeonX1000VADriver.bundle` call `_IOServiceOpen` at all (confirmed by decompiling every function in
both binaries and searching for the string - the only match in each is the import stub itself, calling
nothing). This means the 2D and DVD user-client connections are opened by system components this project
has never downloaded (most plausibly WindowServer for 2D, and DVD Player/QuickTime's own hardware-decode
detection for DVD) - real, useful information about the system's architecture, even though it leaves
`type=2`/`type=3` assigned only by the strong, but not independently verified, process of elimination
(four types, four classes, two confirmed, two accounted for).

## `registerNotificationPort`'s "bad instruction data" dissolves entirely - it isn't a real function

The third cited ceiling: `IOUserClient::registerNotificationPort` at kext address `0x52000` decompiled
to `halt_baddata()` with "Control flow encountered bad instruction data." Dumping the kext's memory
block map (`__text`/`__cstring`/`__const`/.../`EXTERNAL`/`ABSOLUTE`) shows `0x52000` falls inside
Ghidra's synthetic **`EXTERNAL`** block (`0x4e000`-`0x6adff`, `initialized=false`) - the placeholder
region Ghidra invents to host symbols for references the linker never resolved locally (the same
mechanism behind the `IOUserClient::vtable`/`IOAccelerator::vtable` address-table warnings seen at
import time). **This means `registerNotificationPort` is not a real function this kext implements at
all** - it's an external, framework-inherited `IOUserClient` base-class method the driver never
overrides, and Ghidra's "bad instruction data" was simply an artifact of trying to disassemble a
synthetic placeholder that was never real code. This fully dissolves the question rather than just
answering it: there is no driver-specific notification-port behavior to find, because none exists.

## Honest limits

- Types 2/3's exact values remain elimination-based, not call-site-confirmed - closing this fully would
  require decompiling WindowServer or DVD Player/QuickTime binaries, which are not part of this
  project's downloaded set and were not pursued (out of scope: those are large, generic system
  components, not GPU-driver-specific).
- Nothing here was run on real hardware - this entire investigation was resolved by fixing a static-
  analysis tooling problem and then reading real, already-shipped code, per the standing constraint for
  this session (no G5 access).
