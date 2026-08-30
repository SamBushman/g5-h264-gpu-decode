# `ATIRadeonX1000GA.plugin`: the classic QuickDraw "Graphics Accelerator" interface, decoded

Continuing the driver survey enabled by the local Tiger HD mount. This bundle was previously only
referenced by name (`ioreg`'s `IOCFPlugInTypes`, noted as "real but a `.plugin` bundle name... not a
kext filename" in an earlier session) - never before decompiled. It turns out to be architecturally
distinct from everything else this project has examined: a real, COM-style CFPlugIn implementing
Apple's classic QuickDraw hardware-acceleration ("GA") interface - a simpler, separate 2D client of the
same underlying kext, alongside (not part of) the GL path this project's whole PROMO4 investigation has
focused on.

## Real structure: a genuine CFPlugIn/IUnknown-style interface

`_ATIRadeonX1000GAFactory` (the real bundle factory entry point, found via `CFPlugInAddInstanceForFactory`)
allocates a `0x98`-byte instance struct whose first field is a pointer to `_radeonGAInterface` - a real,
static vtable/dispatch-table data symbol. `__QueryInterface` implements the real COM-style
`IUnknown::QueryInterface` pattern (checks the requested interface UUID via `CFEqual`, calls
`AddRef` through the vtable, returns the same object pointer) - textbook Apple CFPlugIn code, the
"device interface" pattern `stage0-dispatch-table.md` noted Apple's own general guidance recommends
over raw `IOServiceOpen` (the GL path uses raw `IOServiceOpen` instead, as already documented - this
plugin is the one place in this whole driver family that actually follows Apple's preferred pattern).

Real vtable-style method names (all real symbols, `__`-prefixed matching a stripped C++ vtable-slot
convention): `_QueryInterface`, `_AddRef`, `_Release`, `_Probe`, `_Start`, `_Stop`, `_Reset`,
`_GetCapabilities`, `_GetBeamPosition`, `_GetBlitter`, `_SetSurface`, `_SetDestination`,
`_AllocateSurface`, `_FreeSurface`, `_LockSurface`, `_UnlockSurface`, `_SwapSurface`, `_WaitSurface`,
`_WaitComplete`, `_Synchronize`, `_Flush`, `_DecodePixelFormat`.

## Real hardware 2D operations - simpler primitives than anything else this project has examined

`_radeonCopy`, `_radeonCopyRegion`, `_radeon3DCopySetup`, `_radeonFill`, `_radeon3DFillSetup`,
`_radeonSolidScanlines`, `_radeonHighlight` - real, named hardware-accelerated 2D blit/fill/copy/
highlight operations. This is the classic QuickDraw acceleration API (`CopyBits`, `FillRect`,
`InvertRect`-style text-selection highlighting) used for ordinary Finder-window/non-Quartz 2D drawing
on this hardware - **not** related to OpenGL, GLSL, or the embedded command-buffer/marker language
this project's whole hang investigation has centered on. A real, separate, much simpler client of the
same kext.

## Real, previously-undocumented external-method selector found

`__GetCapabilities` dispatches on a 4-byte FourCC-style tag (`param_2`): recognizes `'BGRA'`
(`0x42475241`, a real pixel-format FourCC) as a direct capability-flag read, and one other tag
(`0x736d766c`) that triggers a genuinely new external-method call:
```c
_io_connect_method_scalarI_structureI(*(int *)(param_1 + 0xc), 0xf, param_3, 1, 0, 0);
```
**Selector `0xF` (15)** - not seen in any of this project's prior selector cataloguing (previously known:
0, 3, 8, 9, `0x12`, `0xd`). Not yet decoded further, and very likely specific to whatever userClientType
this 2D plugin actually opens (probably `IOATIR5002DContext`, matching the kext's own
`ATIR5002DContext`/`IOATIR5002DContext` class pair already seen in `process_command_buffer`'s sibling
functions - not yet confirmed which literal `userClientType` value selects it).

## Assessment: interesting, real, but not directly relevant to the PROMO4 hang investigation

This is a genuinely different, simpler client architecture (2D blit engine, not 3D/shader pipeline) -
useful context for understanding the full shape of what this kext exposes, and a real example of Apple
using the CFPlugIn pattern it recommends but the GL driver bundle itself doesn't follow. Not pursued
deeper this pass given it's a separate client path from the one this project's actual redesign work
(the corrected cursor field, the fence mechanism, the kernel chain-walk bounds issue) has been
targeting - flagging as a real, catalogued area for later rather than a priority to chase further right
now.
