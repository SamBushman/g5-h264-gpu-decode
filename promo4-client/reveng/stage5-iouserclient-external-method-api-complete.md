# The complete IOUserClient external-method API surface

Direct follow-up to the "unexplored driver surface area" survey: this closes the single gap identified
as most load-bearing for PROMO4's actual goal (a from-scratch userspace client) - the kext's
`IOUserClient` external-method dispatch tables had never been mapped beyond the one previously-known
selector (GL context selector `9`, the fence wait). Done via a fresh, dedicated Ghidra project imported
directly from the locally-held `ATIRadeonX1000.kext.bin` (no hardware needed - pure static analysis).

## The real class hierarchy

Four real, distinct user-client-connectable context classes exist, each an `IO`-prefixed base class
(implementing the bulk of the real external methods) with a further, non-`IO`-prefixed subclass adding
3D/2D/DVD-specific behavior (all of `process_command_buffer`, `write_kernel_context_buffer_regs`, etc.
already decoded this session live in these subclasses):

| Base class (`IO`-prefixed) | Subclass | Real external-method count | `getTargetAndMethodForIndex` @ |
|---|---|---|---|
| `IOATIR500GLContext` | `ATIR500GLContext` | 20 (0-19) + **1 special (20)** | `0x26db0` |
| `IOATIR5002DContext` | `ATIR5002DContext` | 16 (0-15) + 3 extra (16-18) | `0x31990` |
| `IOATIR500DVDContext` | `ATIR500DVDContext` | 10 (0-9) + 12 extra (10-21) | `0x33bf0` |
| `IOATIR500Surface` | `ATIR500Surface` | 19 (0-18), single flat table | `0x3ac80` |

Each `getTargetAndMethodForIndex` returns a pointer into a real, static `IOExternalMethod`-style array
(6 dwords/24 bytes per entry: target=0 (patched live), a constant flags word `0xffff`, the real function
pointer, then 2-3 small integer count fields matching classic `count0`/`count1`(/`count2`) argument-size
metadata) - confirmed by directly reading the raw table bytes and resolving each function-pointer dword
back to its real decompiled symbol. The base classes' tables live at a real named local symbol,
`<Class>::start(IOService*)::methodDescs`, populated once at `start()` time - the subclasses that add
extra selectors (GL's special 20th, 2D's/DVD's extra blocks) have their own separate `methodDescs` at
the subclass level, layered on top of the base table via the offset math already reverse-engineered from
`getTargetAndMethodForIndex` itself.

**Factory selection**: `IOATIR500Accelerator::newUserClient(task*, void*, type, IOUserClient**)` (`0x2070`)
is the real dispatcher an `IOServiceOpen(..., type, ...)` call from userspace lands in - it switches on
`type` (0/1/2/3) and calls one of four vtable slots (`this+0x5d4/0x5d8/0x5dc/0x5e0`) to construct the
actual context object, tagging it with the caller's `task*` and completing the connection via three more
vtable calls (`open`, then two initialization steps). **Which `type` value maps to which of the four
context classes could not be resolved** - those exact vtable slots in `IOATIR500Accelerator`'s own vtable
read back as zero, a real casualty of the same "Relocation type 0xf ... not supported" Mach-O relocations
Ghidra warned about at import time (the same class of static-analysis ceiling already documented for
`AGL.framework` - not a driver-logic dead end, a tooling limit). Honest gap, not pursued further; the
`type` value could plausibly still be recovered by checking what `IOServiceOpen` call site(s) in
`ATIRadeonX1000GLDriver.bundle`/`AGL.framework` actually pass, if a future pass wants to close it (framed
here as a real question with a real, non-hardware test method: grep the userspace driver bundle for the
literal small-integer argument passed to its own `IOServiceOpen`/`IOConnectMethodScalarIStructI`-family
call).

## The complete GL-context external-method table (the one this project has focused on all session)

| Selector | Function | Real args (scalar-in/struct-in/scalar-out-ish, from the table's count fields) |
|---|---|---|
| 0 | `set_surface(m, eIOGLContextModeBits, m, m)` | 4, 4 |
| 1 | `set_swap_rect(l,l,l,l)` | 4, 4 |
| 2 | `set_swap_interval(l,l)` | 4, 2 |
| 3 | `get_config(m*,m*,m*)` | out 3 |
| 4 | `get_status(m*)` | out 1 |
| 5 | `get_surface_size(l*,m*,m*,m*)` | out 4 |
| 6 | `get_surface_info(m,l*,m*,m*)` | in 1, out 3 |
| 7 | `read_buffer(sIOGLContextReadBufferData*,m)` | in 3, variable-size (struct) |
| 8 | `finish()` | 4, 0 |
| 9 | **`wait_for_stamp(m)`** | 4, 1 - **confirmed**: this is the already-known "targeted kernel wait" selector from `stage3-fence-mechanism.md`, now directly named and placed in context |
| 10 | `new_texture(sIOGLNewTextureData*, sIOGLNewTextureReturnData*, m, m*)` | variable in/out |
| 11 | `delete_texture(m)` | 4, 1 |
| 12 | `become_global_shared(m)` | 4, 1 |
| 13 | `page_off_texture(m,m,j,j)` | 4, 2 |
| 14 | `scale_surface(m,m,m)` | 4, 3 |
| 15 | `purge_texture(m)` | 4, 1 |
| 16 | `set_surface_volatile_state(m)` | 4, 1 |
| 17 | `reclaim_resources()` | 4, 0 |
| 18 | `get_data_buffer(j*,m*)` | out 2 |
| 19 | `set_stereo(m,m)` | 4, 2 |
| **20 (special)** | `get_hw_info(m*,m*,m*,m*,m*)` | out 5 |

**Selector 20 was hoped to be a "submit command buffer" trap given how distinctively it's special-cased
in the dispatch code - it is not.** It's a plain hardware-info query (5 output values). This is itself a
real, useful negative result: **there is no explicit "submit"/"flush the ring" external method anywhere
in this table.** Combined with everything already known about the shared-memory command-buffer
mechanism (`clientMemoryForType` mapping type-0 memory directly into userspace, the corrected cursor at
`AGLContext+0x17dc`, and the CP ring's own `CP_RB_WPTR`-driven autonomous consumption already confirmed
in `submit_ring_data`), this is real, direct support for the redesign proposal's existing plan: **the
GPU's command processor consumes new ring content once the write-pointer register is updated - no
kernel trap is needed per submission**, and any real submission call this driver's userspace side makes
(`_gldFinish`/`FUN_0001a0f0`) is doing so by writing that pointer directly into the shared-mapped memory,
not by calling into this table at all.

## The other three tables, briefly

**2D context** (16 base + 3 extra): `set_surface`, `get_config`, `get_surface_info`, `swap_surface`,
`scale_surface`, `lock_memory`/`unlock_memory`, `finish`, `declare_image`/`create_image`/`create_transfer`/
`delete_image`/`wait_image`, `set_surface_paging_options`/`set_surface_vsync_options`/`set_macrovision`,
plus **`read_regs`/`write_regs`/`write_2_regs`** (selectors 16-18) - see below.

**DVD context** (10 base + 12 extra, the richest table): `set_surface`, `get_config`/`get_status`/
`get_surface_size`, `lock_all_buffers`/`unlock_memory`/`write_buffer`, `finish`, `declare_image`/
`delete_image`, then the extra block: `show_buffer`, `dvd_setup_overlay`/`dvd_enable_overlay`/
`dvd_setup_subpicture`/`dvd_enable_deint` (real overlay/subpicture/deinterlace control, consistent with
`stage3-va-driver-is-not-a-decode-shortcut.md`'s AVA finding), **`read_regs`/`write_regs`** again, **and
- directly relevant to the project's overarching H.264 GPU-decode goal - `doIDCT`, `wait_for_stamps`,
`check_stamps`, and `setup_buffers`.**

**Surface** (19, flat table): all lock/unlock/backing-store/shape management for shared display
surfaces (`surface_read`/`write_lock`/`unlock`, `set_shape`/`set_shape_backing`, `get_state`,
`surface_control` - appearing twice, at selectors 16 and 18, pointing to the *same* function address,
a real intentional selector alias rather than two different implementations). Least relevant to the
compute-acceleration goal - this class is about window-server shared-surface bookkeeping.

## Two standout discoveries

### 1. `doIDCT` names and directly confirms the independent hardware IDCT engine

`stage4-real-hardware-idct-engine-found.md` hypothesized an independent hardware IDCT path from its own
separate command ring. This pass found and decoded the actual external entry point:
`ATIR500DVDContext::doIDCT(sATIDVDIDCTInfo*, unsigned long)` (`0x35540`). Real, decoded behavior:
computes per-field (top/bottom, matching interlaced video) and per-plane (luma/chroma, doubling the
chroma pitch) buffer geometry, maintains real double-buffering (ping-pong between two destination
buffer pointers per plane), calls the already-known `map_transfer_to_GART` to ensure the working buffer
is mapped, then calls **`ATIRadeonX1000::submit_idct_buffer_consumed`** - a real, newly-named function
confirming this project's hypothesis that the IDCT engine has its own independent submission path,
separate from the main 3D ring's `submit_buffer`. `wait_for_stamps`/`check_stamps` (selectors 19/20 in
the DVD table) mirror the GL side's fence pair, confirming the IDCT path uses the same real
generation-tag/stamp synchronization architecture already fully understood from the GL context.

**This is a concrete, real, externally-callable API for exactly the kind of hardware-accelerated
decode-stage work PROMO4's broader goal (H.264 GPU-shader decode) is aimed at** - a legitimate, sanctioned
IOKit entry point already shipped by Apple, rather than something requiring raw command-stream
injection at all.

### 2. `read_regs`/`write_regs`/`write_2_regs`: real, kernel-validated raw register access - with a real, honest limit

Both the 2D and DVD contexts expose direct register read/write external methods. Decoded behavior
(identical shape in both classes): each masks the caller-supplied register offset with **`& 0x1ffc`**
before adding it to a per-context MMIO base pointer - a real, deliberate 8KB (0x2000-byte), 4-byte-
aligned safety window, not unrestricted raw MMIO passthrough. Both also check a real "device active"
byte flag (`*(char*)(base+0x80)`) before touching hardware, and `read_regs` additionally validates the
caller's claimed transfer size against the actual buffer size before proceeding - real, deliberate input
validation, a genuine contrast with the embedded command-buffer marker language's confirmed **zero**
validation (`stage3-kernel-side-hang-mechanism-confirmed.md`).

**The honest limit**: the `0x1ffc` window is far too small to reach most of the registers this project
has spent the session mapping. `stage4-complete-register-tracking-state-map.md`'s ~45-register 3D
pipeline map is almost entirely in the `0x2000-0x5000` byte range (`VAP`/`SC`/`GA`/`US`/`TX`/`RB3D`/`ZB`
blocks) - outside this window. What *is* likely reachable in `0x0-0x1ffc` is CP (command processor) and
early display/2D-engine register space - not independently confirmed this pass (would require checking
the low end of the register PDFs against this exact range, not yet done).

**Real, documented (not attempted) hardware-test opportunity this discovery opens up**: for any future
register-value hypothesis that happens to fall within `0x0-0x1ffc`, `write_regs`/`read_regs` is a
strictly safer test path than the marker-language buffer injection that caused this project's two
previous real hangs - it's a sanctioned, validated, narrow-scope IOKit call rather than raw pointer
writes into a driver-private cursor field. Test method (documented only, not run - no G5 access this
session): open a 2D or DVD context via `IOServiceOpen` (once the real `type` value is known - the one
gap this pass didn't close), call `write_regs`/`read_regs` with an offset inside `0x0-0x1ffc`, and
observe the read-back value or a real, benign hardware effect, with the same single-conservative-write,
verify-before-second-write discipline this project has used for every prior hardware step.

## Honest limits

- The `type` argument mapping (`IOServiceOpen`'s connection-type value → which of the four context
  classes gets constructed) is unresolved due to zeroed-out vtable slots from unsupported Mach-O
  relocations - a real tooling ceiling, with a concrete non-hardware follow-up test method noted above.
- The exact semantics of the method-table's flags word (`0xffff`, constant across every entry) and the
  precise meaning of each of the 2-3 small integer count fields per entry were not derived from any
  header/documentation - inferred only by shape-matching against the classic `IOExternalMethod`
  convention. Good enough to know "this is a real, bounded-argument external call," not precise enough
  to hand-construct a call without also checking the corresponding userspace-side caller (not attempted
  this pass).
- Nothing in this document was run against real hardware. Every finding is static analysis of the
  locally-held kext binary only, per the standing constraint for this session (no G5 access).
