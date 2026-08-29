# Stage 3 continued: real "attach drawable" selectors found - a more direct path than the opcode language

Follow-up to `stage3-embedded-opcode-language.md`. That thread's opcodes (texture load, surface build,
fast clear) all assume a render target is *already* attached to the context. Investigated where
attachment itself happens - found it's a separate mechanism entirely, simpler in shape than the
embedded opcode language: a small set of plain external-method selector calls, not memory-mapped or
opcode-embedded.

## Real finding: `_gldAttachDrawable`

Decompiled `_gldAttachDrawable` (GL driver bundle) - the real function AGL's own `aglSetPBuffer`/
window-attach calls internally (confirmed by name and by its real branching on `param_2` values
matching known AGL surface-type constants: `0x50`/`0x5a`=window-family attach, `0x36`=PBuffer attach,
`0x35`/`0`/`0x5b`=other real cases). **No internal callers found** in this binary - it's a public entry
point AGL calls directly by symbol from outside the bundle, so exact real caller-supplied parameter
values for the PBuffer path aren't recoverable by static xref analysis alone; the sequence of kernel
calls it makes is real and fully visible regardless.

## Real new external-method selectors found (beyond the already-confirmed 3, 8, and `0x12`)

| Selector | Real shape (as called) | Real context |
|---|---|---|
| **0** | `structureI`, 4-dword input | Called from **both** the window and PBuffer attach paths - real, strong candidate for "attach this drawable/surface to the context." Params: `{surface-related value, flags, a byte from param_4>>8, a byte from param_4&0xff}`. |
| **3** | `scalarI_scalarO`, 0 in / 3 out | Already confirmed (Stage 0/1) - also called again inside the PBuffer attach sequence itself, after selector 0. |
| **4** | `scalarI_scalarO`, 0 in / ~1-2 out | Called right after selector 3 in the PBuffer attach sequence - result's bit 0 gates whether cached dimensions are reused or refreshed. |
| **5** | `scalarI_scalarO`, 0 in / ~2 out | Called right after selector 0 succeeds, before selector 3, in the PBuffer attach sequence. |
| **6** | `scalarI_scalarO`, 1 in / 1 out | Called early in the window-attach path with a caller-supplied value (`param_3[2]`) - looks like a pixel-format/capability query. |
| **7** | `scalarI_scalarO`, 2 in / some out | Called in the PBuffer path, before selector 0, with a byte value and a fixed `0`. |
| **0xe** (14) | `structureI`, 3-dword input | Conditional call inside the window-attach path, gated on an internal flag (`param_1+0x145`). |
| **0x10** (16) | `structureI`, 1-dword input | Called right after selector 0xe (or selector 0 in the simpler path) - takes a single byte value. |

## Real, concrete sequence for PBuffer attach (the case matching every AGL context this whole project
has used - `aglCreatePBuffer`/`aglSetPBuffer`)

```
(if not already open) IOServiceOpen(secondary service, type=0)   <- a REAL, separate connection,
                                                                       different from the main context
selector 7  (scalarI_scalarO, 2 in)
selector 0  (structureI, 4 dwords)          <- the real "attach" call
selector 5  (scalarI_scalarO, 0 in)
selector 3  (scalarI_scalarO, 0 in / 3 out) <- same query already confirmed safe (Stage 0/1)
selector 4  (scalarI_scalarO, 0 in)
```
Followed by a real CPU-side `malloc` sized `bytesPerPixel * align8(width) * height` - almost certainly
the CPU-visible readback buffer AGL uses for `glReadPixels`-style operations, confirming this sequence
really does establish a real, complete, readable render target.

## Honest status

This is a real, more direct candidate path than continuing to chase the embedded opcode language and
its format table: selector 0 looks like the actual "bind a render target" call, needed before any of
the texture/surface/clear opcodes from the previous entry would have anything to act on. The real,
remaining uncertainty is the *exact* correct values for selector 0's 4-dword payload and the secondary
connection's real service identity - not yet resolved via static analysis, since `_gldAttachDrawable`
has no in-binary callers to inspect for concrete argument values. Real next step: attempt selector 0
empirically with reasoned-but-unconfirmed parameters and observe the real result code. This is a
meaningfully safer kind of "guess and test" than raw PM4/register writes were - external-method calls
go through the kernel's own validated dispatch and argument-count checking, and fail safely with a real
`IOReturn` error code on malformed input rather than risking undefined hardware behavior.
