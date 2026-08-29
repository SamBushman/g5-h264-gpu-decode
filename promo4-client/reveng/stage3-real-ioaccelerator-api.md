# Stage 3: the real, public, documented IOAccelerator API - the biggest pivot in this thread

Follow-up to `stage3-dynamic-trace-attach.md`, which traced the real PBuffer-attach sequence into
`IOAccelCreateSurface`/`IOAccelSetSurfaceFramebufferShape` calls. Rather than reverse-engineer those
functions' own internal protocol from scratch, checked whether Apple documents them directly - **they
do**. This is real, public, standard Apple API, not a private or obscure mechanism, and it changes the
whole approach for the rest of this thread.

## Real, public headers found

`/System/Library/Frameworks/IOKit.framework/Versions/A/Headers/graphics/`:
`IOAccelClientConnect.h`, `IOAccelSurfaceConnect.h`, `IOAccelTypes.h` - genuine Apple Public Source
License headers, shipped as part of the standard IOKit framework, not extracted or reverse-engineered.

## Real, decisive findings

- **`kIOAcceleratorClassName = "IOAccelerator"`** - the real, standard IOKit service class name for
  this whole subsystem. Given `ATIRadeonX1000`'s own `ioreg` entry (found back in Stage 1) already
  showed `"IOMatchCategory" = "IOAccelerator"`, this strongly suggests `ATIRadeonX1000` itself is where
  this class registers on this hardware - the same kext, a different, real, documented user-client type.
- **`kIOAccelSurfaceClientType = 0`** (from `eIOAcceleratorClientTypes`) - a real, DIFFERENT
  `IOServiceOpen` user-client type than the `type=1` (`IOATIR500GLContext`) used throughout this entire
  project so far. Type 0 is the real, public, documented "surface" client - exactly the render-target
  management interface this whole Stage 3 thread has been trying to hand-derive.
- **Real, complete selector enum** (`eIOAccelSurfaceMethods`, values 0-17): `kIOAccelSurfaceReadLockOptions`,
  `..WriteLockOptions`, `..GetState`, `..Read`, `..SetShapeBacking`, `..SetIDMode`, `..SetScale`,
  **`kIOAccelSurfaceSetShape=9`**, **`kIOAccelSurfaceFlush=10`**, `..QueryLock`, `..ReadLock`/`WriteLock`
  (+Unlock), `..Control`, `..SetShapeBackingAndLength`. These are real, documented, stable selector
  numbers for a *different* user-client type than the one this whole project has decompiled so far -
  not something to guess from disassembly.
- **Real structure definitions** (`IOAccelTypes.h`): `IOAccelBounds` (`x,y,w,h` as `SInt16`),
  `IOAccelSurfaceInformation` (real surface memory layout - `address[4]`, `rowBytes`, `width`,
  `height`, `pixelFormat`, `flags`, `colorTemperature[4]`, `typeDependent[4]`), `IOAccelSurfaceScaling`,
  `IOAccelID` (a plain `SInt32`).
- **Real mode/option bit enums** (`IOAccelSurfaceConnect.h`): `kIOAccelSurfaceModeColorDepth8888=4`
  (matches this project's own RGBA8 framebuffers throughout), `kIOAccelSurfaceShapeNone=0`,
  `kIOAccelSurfaceShapeIdentityScaleBit`, etc. - real, meaningful constants for configuring a surface,
  not opaque magic numbers.

## Why this matters more than continuing to trace `IOAccelCreateSurface`'s internals

`IOAccelCreateSurface`, `IOAccelFindAccelerator`, `IOAccelSetSurfaceFramebufferShape`, and friends
(confirmed real, exported, linkable symbols via the live gdb trace in the previous entry - `service=
0x1f07` was identical and stable across three separate real runs, `type=0` likewise) are themselves
just convenience wrappers **around this same real, documented `IOAccelSurfaceClientType`/selector
protocol** - Apple ships the real client library that speaks it. Rather than reverse-engineer
`IOAccelCreateSurface`'s own internal IOKit calls from raw disassembly (more guesswork, more risk of
getting an undocumented internal detail wrong), the direct, well-grounded path is either:

1. **Link against and call the real `IOAccel*` functions directly** - they are real, exported symbols
   in a system library this project's own test programs can link against, exactly as Apple's own CGL
   implementation does. No protocol reverse-engineering needed for surface creation at all.
2. **Or, open a raw `IOServiceOpen(service, task, 0, &conn)` connection directly** (type 0, not 1) and
   use the real, documented selectors from `eIOAccelSurfaceMethods` - still real and grounded, just
   lower-level.

Either path replaces this whole thread's hand-reverse-engineered, undocumented-magic-number approach
(the embedded opcode language, the guessed selector-0 payloads) with real, Apple-documented, stable
API surface for the specific piece that was missing: creating and shaping a real render target.

## Honest status

This is the most significant course-correction in the whole Stage 3 investigation: what looked like it
required either a huge from-scratch 3D pipeline bring-up or continued blind guessing at an undocumented
protocol turns out to have a real, public, Apple-documented API sitting right next to it, discovered by
checking whether the real function names (found via the live gdb trace) corresponded to shipped
headers - they did. Not yet implemented or tested on real hardware this pass; the real next step is
building a test program that either links `IOAccel*` directly or opens a `type=0` connection and drives
it through the real, documented `kIOAccelSurfaceSetShape`/`kIOAccelSurfaceFlush` selectors.
