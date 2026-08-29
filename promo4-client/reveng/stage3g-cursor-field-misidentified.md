# Stage 3g post-mortem: the "cursor" field used for live injection was actually the chain-link pointer

Real, decompile-grounded correction to the offsets `stage3g_real_injection.c`/`stage3h_header_dump.c`
used, found via pure static RE (Ghidra, zero hardware risk) while the G5 was down for a physical
power-cycle after the second live hang. This resolves *why* a write that passed the alignment/header
safety check still hung the machine.

## What the live gdb-found offsets actually are

Both stage3g/h treated `AGLContext+0x17d8` as the live write cursor and `AGLContext+0x17e4` as the
mapped buffer base. Decompiling this driver's own real flush machinery in
`ATIRadeonX1000GLDriver.bundle` shows this was wrong for the first of those two fields.

`_gldFinish`/`_gldFlush` (real decompile, addresses `0001a5e0`/`0001a5c0`):
```c
void _gldFinish(int param_1) {
  if (*(int *)(param_1 + 0x1e4) + 0x28U < *(uint *)(param_1 + 0x1dc)) {
    FUN_0001a0f0(param_1, 0x1000000);
  }
  do { ... } while (io_connect_method_scalarI_structureI(..., 8, ...) == -0x1ffffd2a);
}
```
This confirms `param_1+0x1dc` is the **real** write cursor (compared against `base+0x28` to decide
"nearly full") and `param_1+0x1e4` is the **real** base - consistent with what was assumed for base.

`FUN_0001a0f0` (the real submit/flush/remap function, address `0001a0f0`) is where the field at
`param_1+0x1d8` shows up - and it is NOT the cursor:
```c
puVar5 = *(uint **)(param_1 + 0x1d8);                 // "previous" chain-link pointer
*puVar5 = (int)puVar4 - (int)puVar5 >> 2 | *puVar5;   // OR's a relative dword-distance into it
...
*(int *)(param_1 + 0x1d8) = iVar3 + 0x1c;             // after a flush, reset to newBase + 0x1c
```
`param_1+0x1d8` is a separate bookkeeping field - a link to the most recent marker/tail record,
used by the driver-private "extended opcode" chain (see `stage3-embedded-opcode-language.md`) so the
kernel's `process_command_buffer` can walk from one record to the next across buffer boundaries. It is
**only ever touched inside `FUN_0001a0f0`**, never inside the plain `_gldFinish`/`_gldFlush` fast path.

## The real field layout, and where the live offsets actually point

Driver-local field order (four consecutive dwords, byte offsets from `param_1`):
| Offset | Field |
|---|---|
| `+0x1d8` | chain-link "previous record" pointer (private bookkeeping, `FUN_0001a0f0`-only) |
| `+0x1dc` | **real write cursor** |
| `+0x1e0` | end/limit boundary |
| `+0x1e4` | buffer base |

The live `AGLContext`-relative addresses found via gdb line up with a **constant +0x1600 offset**
applied to this same layout - confirmed exactly by `+0x1e4` (driver-local base) mapping to the
gdb-found `AGLContext+0x17e4` (also base): `0x1e4 + 0x1600 = 0x17e4`, exact match. Applying the same
`+0x1600` constant to the other three fields:

| Driver-local | Real field | AGLContext-relative (corrected) |
|---|---|---|
| `+0x1d8` | chain-link pointer | **`+0x17d8`** (what stage3g/h actually read/wrote as "cursor") |
| `+0x1dc` | **real write cursor** | **`+0x17dc`** (never touched by stage3g/h at all) |
| `+0x1e0` | end/limit | `+0x17e0` |
| `+0x1e4` | base | `+0x17e4` (correctly identified) |

This is confirmed numerically, not just by pattern-matching the offset arithmetic:
`FUN_0001a0f0` sets the chain-link field to `newBase + 0x1c` after every flush through it - and that
is *exactly* the value stage3h's read-only dump measured for "cursor" in the shader-bound test case
(`base+0x1c`). The plain-clear case's "`base+0x20`" reading is equally explained: a plain fixed-
function clear never calls `FUN_0001a0f0` (its buffer never gets full enough), so the chain-link field
is left at whatever the kernel's own `init_command_buffer_header` set it to originally - which,
per the Stage 2-prep decompile, zeros bytes 0-0x1f and only explicitly sets 0x10/0x14/0x18/0x1c/0x20,
never touching the chain-link's own resting value coincidentally so it can read as a stale/leftover
pointer that happened to equal `base+0x20` in that run.

## What this means actually happened during both hangs

Both `stage3g_real_injection.c` runs read/wrote `AGLContext+0x17d8` believing it was the cursor. In
reality:
- The **safety check** ("is this offset past the header") was checking the **chain-link pointer's own
  target address**, not the true cursor - a coincidentally plausible-looking number, not a meaningful
  safety property.
- The **write** itself landed at wherever the chain-link pointer happened to point (which, in the
  "settled" run, happened to be a valid position inside the real command buffer, base+0x20) - so the
  raw PM4 payload bytes may have been physically fine.
- But `inject_at_live_cursor`'s final step, `*(ctx+0x17d8) = newCursor`, **overwrote the real chain-
  link pointer itself** with an arbitrary advanced address that has no relationship to what
  `FUN_0001a0f0`'s own protocol expects there (a pointer to a real record's length/type field, not a
  bare "next free byte" address).
- The next real flush cycle through `FUN_0001a0f0` (very plausibly triggered by re-binding the GLSL
  program and drawing again, since that path uses the embedded-opcode/marker mechanism) executed
  `puVar5 = *(uint **)(param_1 + 0x1d8); *puVar5 = ... | *puVar5;` against our **corrupted** pointer -
  writing through it, and feeding a resulting garbage relative-distance value into the marker chain
  the kernel's `process_command_buffer` walks at consumption time. A corrupted chain walk in kernel/
  GPU-command-processor code hanging the ring (and, per the user's report, blanking the display) is a
  fully consistent explanation for a real, display-affecting, SSH-unresponsive hang - not a simple
  invalid-memory-access crash, which is exactly why it manifested as a hang rather than a clean error.

## Definitive confirmation: the actual initialization code

Found the real one-time context-creation initializer, `FUN_0002c790` (called from the end of
`_gldCreateContext`), which removes any doubt - this is the literal, unconditional setup code, not an
inferred pattern:
```c
iVar8 = *(int *)(param_1 + 0x1e4);              // iVar8 = base
*(int *)(param_1 + 0x1dc) = iVar8 + 0x20;       // CURSOR := base + 0x20
*(int *)(param_1 + 0x1d8) = iVar8 + 0x1c;       // CHAIN-LINK := base + 0x1c
*(int *)(param_1 + 0x1e0) = iVar8 + 0x20 + *(int *)(iVar8 + 0x10) * 4 + -0x94;  // end/limit
*(undefined4 *)(iVar8 + 0x1c) = 0;              // header's own +0x1c field explicitly zeroed
```
Cursor and chain-link are set to two *different* values (`base+0x20` vs `base+0x1c`) at the exact same
moment, by the exact same function - conclusive proof they are distinct fields with the roles
described above, not an artifact of pattern-matching two separately-observed runs. This also explains
why the plain-clear test read the header's own `+0x1c` byte as `1` (the kernel's untouched default):
that path evidently never routes through this initializer or through `FUN_0001a0f0` - both of which
unconditionally zero it - so a third, simpler, not-yet-identified buffer-management path must be what
plain fixed-function clears use, one that never touches the chain-link machinery at all because it
never needs the embedded-opcode/marker consumer. That third path is a reasonable next target if this
investigation continues.

## Definitive confirmation, part 2: the exact call site that actually corrupted the chain

Traced the real call path from `glUseProgramObjectARB(prog)` (re-binding the already-used program,
the step that immediately preceded the second hang). `_gldUpdateDispatch` (address `00022290`) calls
`FUN_0001bac0` (address `0001bac0`) whenever a dirty-state bit is set (`(*param_3 & 0x180) != 0` at
`_gldUpdateDispatch`'s own line `if ((*param_3 & 0x180) != 0) { ... FUN_0001bac0(param_1); ... }`) -
exactly the kind of state change a program rebind would trigger. `FUN_0001bac0`'s real decompile:
```c
puVar4 = *(uint **)(param_1 + 0x1d8);                       // read the CURRENT chain-link pointer
*puVar4 = (int)puVar2 - (int)puVar4 >> 2 | *puVar4;          // dereference it and WRITE through it
*(undefined4 **)(param_1 + 0x1d8) = puVar2;                  // then advance chain-link to a new record
*puVar2 = 0x41000000;                                        // writes a new embedded-opcode marker
...
if (*(int *)(param_1 + 0x1e4) + 0x28U < *(uint *)(param_1 + 0x1dc)) {
    FUN_0001a0f0(param_1, 0x1000000);                         // separate, real cursor-based flush check
}
```
This is the exact, concrete mechanism: `stage3g_real_injection.c`'s final step overwrote
`AGLContext+0x17d8` (`param_1+0x1d8`) with an arbitrary "advanced pointer" value that has no
relationship to a real chain-link target. The very next `glUseProgramObjectARB(prog)` call reached
this code, read that corrupted value into `puVar4`, and **immediately dereferenced and wrote through
it** (`*puVar4 = ... | *puVar4`) - writing a bogus, essentially-random relative-distance value into
whatever memory the corrupted pointer happened to still validly point to (44 bytes past a previously-
real address inside the same large mapped buffer, so no immediate fault). That corrupted distance
value becomes part of the real command-buffer content the kernel's `process_command_buffer` later
walks as a chain of markers - a corrupted chain-walk hanging the GPU's command processor (and, per the
user's report, blanking the display) is now a fully traced, not merely inferred, explanation. New
marker value found in the same function worth adding to `stage3-embedded-opcode-language.md`'s table:
`0x41000000` (writes real per-surface format state, byte read from `surface+0x38`, similar shape to
the already-mapped `0x46000000` fast-clear opcode - not yet given a real operation name).

Also checked and ruled out the simpler "rebinding directly triggers `FUN_0001a0f0`" hypothesis:
`_gldUpdateDispatch` and `_gldDestroyPipelineProgram` never call it directly, and neither do the two
sibling functions called alongside `FUN_0001bac0` (`FUN_00017260`, `FUN_00007d50` - both decompiled,
neither touches the buffer fields at all). `FUN_0001bac0` is the one and only real link in this chain.

## Bonus: resolves Stage 0's long-open "memory type 2 purpose" question

While tracing the above, confirmed something `stage0-dispatch-table.md` flagged as "not yet determined
... only touched by `_gldCreateContext` so far": memory type 2's mapped base is stored at word-offset
`0x82` from the renderer struct (`puVar6[0x82]`, i.e. byte offset `0x208` - `_gldCreateContext`:
`IOConnectMapMemory(conn, 2, task, puVar6+0x82, puVar6+0x83, 1)`). `FUN_00017310`'s embedded-opcode
handler for marker `0x29b` (real code, already decompiled for the chain-link investigation above)
dereferences `*(int *)(param_1 + 0x208)` repeatedly as a growth-allocator control block (fields at
`+8`/`+0xc`), tracked against a real running position/limit at `param_1+0x1f4`/`+0x1f8`/`+0x1fc`, and
calls external-method selector `0x12` (already known: "vertex/index buffer growth allocation") to get
more space from the kernel when the local allocator runs low. **Memory type 2 is the vertex/index
buffer pool** - confirmed via an exact address match between `_gldCreateContext`'s allocation site and
`FUN_00017310`'s real consumer, not inference.

## Fourth independent confirmation of the `+0x1600` delta

Every decompiled function passes `*(undefined4 *)(param_1 + 4)` as the IOKit connection handle to
every `io_connect_method_*`/`IOConnectMapMemory` call (`_gldFinish`, `FUN_0001a0f0`, `FUN_00017310`,
`FUN_00019db0` alike). `0x4 + 0x1600 = 0x1604` - an exact match to the gdb-found "connect handle"
field this project already trusted. Combined with the base field (`0x1e4`/`+0x1600=0x17e4`) and the
chain-link field (`0x1d8`/`+0x1600=0x17d8`), this is now four independent fields agreeing on the exact
same constant delta - strong evidence `AGLContext` and the driver's internal renderer struct are the
same underlying allocation, offset by a fixed `0x1600`, not reached through a separate pointer chase
each time.

Also decompiled the one remaining unexamined embedded-opcode handler, `FUN_00019db0` (marker `0x1fe`,
gated on a real feature-object check at `param_1[0x3e]`). Same `base+0x28 < cursor` check confirmed yet
again (expressed via word-indexed pointer arithmetic - `param_1[0x79]`/`param_1[0x77]`, i.e. byte
`0x1e4`/`0x1dc` - rather than byte-offset casts, an independent Ghidra type-inference path landing on
the same real offsets). Also surfaces a previously-undocumented external method, **selector `0`** on
this `userClientType=1` GL-context connection (`io_connect_method_scalarI_structureI(param_1[1], 0,
&local_28, 4, 0, 0)` - a 4-byte capability/dither-mode bitmask, gated on gamma-range float comparisons
against `param_1[0xa95]`/`[0xa96]`). Not central to the injection-safety question, but rounds out
opcode coverage and is worth folding into `stage0-dispatch-table.md`'s selector list later.

## Real, concrete correction for any future attempt

- The real write cursor is at `AGLContext+0x17dc`, not `+0x17d8`. Any future injection design should
  read/advance `+0x17dc`, and the "past the header" safety check should be computed against `+0x17dc`
  and `+0x17e4` (base), not `+0x17d8`.
- `+0x17d8` (chain-link) should be left alone entirely unless a future design also correctly implements
  `FUN_0001a0f0`'s own OR-encoded relative-distance write into it - touching it casually (even just to
  "advance" it) is now understood to be a real, direct path to a driver-state-corrupting hang,
  independent of whether the raw payload write itself was safe.
- The end/limit field at `+0x17e0` is a third, previously-unused field worth reading too - it is what
  `_gldFinish`/`_gldFlush` actually compare the cursor against (`base+0x28`, not a fixed universal
  constant - re-derive it live rather than hardcoding `0x28`).
- Not yet re-tested on hardware - this is pure static analysis produced while the G5 was down for its
  second physical power-cycle this project. The next live attempt should target `+0x17dc` specifically,
  and should still keep the existing alignment/bounds abort logic (updated for the corrected field),
  since even with the right field, an unverified value is still a real risk.
