# The kernel-side hang mechanism, fully confirmed: an unbounded chain-walk loop

The user mounted `Tiger HD` locally (read-only) and copied out the kext binary
(`ATIRadeonX1000.kext`) plus `ATIRadeonX1000GA.plugin`, `ATIRadeonX1000VADriver.bundle`, and
`libGLProgrammability.dylib` - files this project could only reach before via the G5 itself, which has
been down since the two live hangs earlier in this session. This finally allows kernel-side
verification of the hang mechanism `stage3g-cursor-field-misidentified.md` diagnosed from the client
side alone.

## The real function: `ATIR500GLContext::process_command_buffer`

Decompiled in full (`0002b820`, 3,315 lines - a large, real function, consistent with the "6,354-line"
figure an earlier session cited for this-plus-`write_r500_3d_blit_state_packet` combined). This is the
kernel's real consumer of the embedded "extended opcode" marker language
(`stage3-embedded-opcode-language.md`) - the function that walks the chain of records the client writes
into the command buffer.

**Setup** (top of the function):
```c
puVar69 = *(ulong **)(this + 0x108);
local_1c0 = *(int *)(this + 0xe0) + 0x20;         // matches the client's own "cursor = base+0x20" convention exactly
puVar65  = (ulong *)(*(int *)(this + 0xe0) + 0x1c); // the kernel's OWN chain-walk pointer - same +0x1c relationship already found client-side
local_1c4 = local_1c8 + puVar69[5];                // computed "end of buffer" bound
```
`this+0xe0` is the kernel's own cached "current buffer base" - the same semantic role as the client-side
base field this project already confirmed (`AGLContext+0x17e4`), now seen independently from the kernel
side with the identical `+0x1c`/`+0x20` offset relationships.

## The real loop, and the real bug: NO bounds check

```c
do {
    uVar73 = *puVar65;               // read the marker dword at the current walk position
    uVar63 = uVar73 & 0xffffff;      // low 24 bits: the "distance to the next record", in dwords
    iVar52 = uVar63 * 4;
    uVar38 = uVar73 & 0xff000000;    // top byte: the opcode
    ... (huge dispatch on the top byte - texture ops, surface ops, fast-clear, etc.) ...

LAB_00031340:
    uVar55 = local_384 + uVar63;
    puVar65 = (ulong *)((int)puVar65 + iVar52);   // ADVANCE by uVar63*4 bytes, unconditionally
    if (uVar63 == 0) {                             // the ONLY exit condition
        ... finalize, write results into *param_1, return ...
    }
} while (true);                                    // otherwise loop forever
```

**`local_1c4` (the computed "end of buffer" bound) is assigned once at the top of the function and
never read again anywhere in these 3,315 lines.** Confirmed by direct search - it appears exactly
twice in the whole decompile: its declaration and its one assignment. **There is no bounds check on the
chain-walk loop at all.** The loop's only exit condition is reading a marker dword whose low 24 bits
happen to be exactly zero; every single other outcome - including reading raw, non-marker data such as
this project's own injected PM4 register-write dwords, or a description of the earlier corrupted
chain-link pointer treated as data - just becomes "advance by N*4 bytes and keep going," with `N`
derived from effectively arbitrary bits of whatever was actually written there.

## What this means for both of this project's real hangs

This is now a complete, kernel-verified explanation, not an inference from client-side evidence alone:

- **First hang** (writing at an unverified `base+0x1c`, mid-header): whatever ended up at the walk
  position was not a valid marker dword with a sensible low-24-bit distance and top-byte opcode. The
  loop advanced by a essentially-arbitrary byte count derived from garbage and kept dispatching on an
  arbitrary "opcode" byte, quite possibly walking into memory outside the real command buffer entirely,
  with the kernel dispatching on whatever opcode-shaped byte pattern happened to occur next - a
  fully plausible route to a genuine kernel-context hang or GPU-command-processor lockup, matching the
  real, observed display-blanking, SSH-unresponsive symptom.
- **Second hang** (corrupting the chain-link pointer, `AGLContext+0x17d8`, via `stage3g`'s final write):
  the next real flush (`FUN_0001bac0`, already traced client-side) wrote a bogus relative-distance value
  through that corrupted pointer. Since this kernel loop has no independent way to validate that
  distance is sane, it would happily walk by whatever bogus amount ended up there, with the exact same
  unbounded, no-recovery consequence.

Both hangs reduce to the same underlying kernel-side fact: **this marker-chain walk trusts its input
completely.** There is no defensive check anywhere in `process_command_buffer` against a corrupted or
malformed chain - once a bad value is loaged into it, the only recoverable outcomes are "the low 24
bits of essentially-random data happens to be zero soon" (in which case it can even proceed to a
*normal-looking* return, having read no telling how much garbage first) or a hang.

## Real, new opcode range found along the way

The loop's very first branch, not previously mapped (`stage3-embedded-opcode-language.md`'s table only
covered opcodes `0x3a`-`0x46`):
```c
uVar75 = (uVar73 >> 0x18) - 6;
if (uVar75 < 0x10) {   // top byte in [0x06, 0x15] - 16 consecutive values
    iVar59 = uVar75 * 4;
    pVVar70 = *(VendorTextureBuffer **)(this + iVar59 + 0x2a4);   // per-unit texture-slot table (0x2a4 + unit*4)
    if (pVVar70 != NULL) IOATIR500GLContext::remove_texture_from_stream(this, pVVar70);
    ...
}
```
**Markers `0x06000000` through `0x15000000` are a real, previously-unmapped 16-entry family - "unbind/
rebind the texture currently attached to unit N"** (`N = topByte - 6`), matching R5xx's real 16 texture
units. A genuine, concrete addition to this project's opcode catalog.

## Real, concrete design implication for any future injection attempt

Given the walk has zero bounds checking and zero validation of what it reads, **any future attempt to
write raw content into this buffer must itself guarantee a well-formed chain** - either a real, valid
marker dword (correct low-24-bit distance to the next real record, valid top-byte opcode) at every
position the walk could reach, or a genuine `0` low-24-bits terminator placed correctly. Writing "just
some real PM4 register-writes" without also satisfying this separate, softer-level "chain of markers"
protocol is fundamentally unsafe on this exact code path - not because of a specific field mixup (already
fixed conceptually), but because the kernel's own consumer has no way to notice or recover from
malformed input at all. This is a stronger, more general conclusion than the earlier
`stage3g-cursor-field-misidentified.md` finding: even with the CORRECT cursor field and a byte-for-byte
"safe-looking" raw PM4 payload, if that payload's own top-byte happens to look like a
recognized-but-wrong opcode, or its assumed-absent low-24-bit "distance" field is nonzero garbage, the
exact same unbounded walk and hang risk applies.

Not re-tested on hardware - pure static kernel-binary analysis, now possible without the G5 thanks to
the local Tiger HD mount. G5 itself still down.
