# Stage 3: real scope assessment - why a fully self-contained draw is a different-scale undertaking

Follow-up to `stage3-resolution-per-context-state.md`, which identified the real path forward for a
decisive native-ISA reproduction test: a fully self-contained draw issued through one connection
end-to-end (real vertex/rasterizer/render-target setup plus a genuine draw-primitive PM4 packet), since
cross-context injection cannot work (per-context register shadow/restore state). This note documents
real investigation into what that would actually require, and why it's a materially different scope of
work than anything else in this thread - a concrete finding, not a guess.

## Real groundwork already in place

- The real PM4 draw-primitive packet exists and is documented: `3D_DRAW_IMMD_2` (`IT_OPCODE=0x35`,
  AMD's R5xx doc §6.2.3.5) draws primitives from vertex data embedded directly in the packet (no
  separate vertex-buffer memory needed), governed by a `VAP_VF_CNTL` header dword (primitive type,
  vertex count in bits 31:16).
- The real US (fragment shader) instruction encoding and load mechanism from Stage 3's first attempt
  remain valid and reusable - nothing about that work needs to be redone.

## Real complexity found in the kext's own draw-adjacent code

Decompiled `ATIR500GLContext::process_command_buffer` (the real function that runs a client's
submitted render commands) and `ATIR500GLContext::write_r500_3d_blit_state_packet` (which Apple's own
driver uses to construct a real, working 3D-engine packet for a blit operation - the closest real,
working reference this project has for "how does this driver actually set up a real draw"). Both are
large (the full decompile file is 6,354 lines), and `write_r500_3d_blit_state_packet` alone computes:

- Real render-target/surface offset arithmetic involving per-surface pitch, tiling mode, and a lookup
  table at a fixed data address (`DAT_0004d2e0`) indexed by surface format - not a fixed formula, a
  real per-format table this project hasn't characterized.
- A call to `compute_sc_hyperz_en` and `compute_zb_bw_cntl` - real, separate functions computing
  Hierarchical-Z and Z-buffer bandwidth-control register values, an entire real subsystem (this
  project's earlier work found the AMD doc's own §9 HiZ section - real, but not yet studied in depth).
- Real texture-offset computation via `GetTextureOffset`, gated on whether a texture buffer is
  currently bound.
- Real conditional logic selecting between two different state-computation paths depending on an
  internal buffer-chaining flag (`this+0x3bc`), each with its own real pitch/tiling arithmetic.

This is not "a handful of register writes to reach a known-good state" - it's a real, substantial
internal driver subsystem (surface tiling, hyper-Z, render-target format-dependent offset computation)
that Apple's own driver treats as complex enough to warrant dedicated, separately-tested functions.
Reproducing an equivalent by hand, from the AMD PDF spec alone with no reference implementation to
check against for the parts NOT already decompiled (viewport, vertex format, rasterizer/scan-converter
enable are still undocumented in this project's own findings), would mean assembling many
newly-researched, cross-interacting registers with no prior validation - a materially different risk
profile than every previous real hardware action in this thread, each of which either replicated an
exact, already-decompiled real sequence (Stage 1, 2a) or used a value independently confirmed safe by
two real sources (Stage 2b's Type-2 filler). AMD's own doc separately documents a real, specific hang
risk on a related register group (`PS3_VTX_FMT`/`PS3_TEX_SOURCE`, §10.9.5: "can cause bad textures or
hangs in R5xx chips") - a concrete, real illustration of the kind of mistake this territory allows for.

## Real, definitive confirmation: no scratch memory region exists among the four known types

Checked `clientMemoryForType`'s real handlers for the two remaining memory types not yet fully
characterized. **Type 2 is a "context buffer"** (`init_context_buffer_header`, a
`VendorContextBufferHeader`, real fixed size `0x8000` - matching the exact size measured empirically
in Stage 2a) - real, structured, kernel-managed state, not general-purpose memory (almost certainly
related to the per-context register shadow/restore mechanism `store_reg` revealed). Type 4's handler
does dynamic AGP-backed growth (`FUN_0000aee8` with a doubling pattern) consistent with, not
contradicting, Stage 0's fence-region finding - a different layer of the same subsystem. **All four
real memory types (0=embedded calibration plist, 1=command buffer, 2=context/state buffer, 4=fence
region) are confirmed real, specific, actively-used driver subsystems - none is a general-purpose
scratch buffer available to render into.** The switch's `default` case returns `kIOReturnBadArgument`
for any other type value - there is no fifth type to try.

This is a real, concrete, structural blocker for a self-contained draw, not just elevated complexity:
this client interface's only memory-mapping mechanism (`IOConnectMapMemory` across these four known
types) has no room in it for an ad hoc render target. A real attempt would need to find how
`VendorTextureBuffer`/surface memory actually gets allocated instead - real symbols already seen in
this kext (`allocVendorTextureBuffer`, `addTransferToGART`) suggest a separate mechanism, likely
through additional external-method selectors not yet decompiled - genuinely new discovery work, not a
known gap to fill in with more careful register programming.

## Honest assessment

A fully self-contained draw is achievable in principle - the real packet format and load mechanism are
documented and already understood - but safely assembling the surrounding pipeline state (viewport,
vertex format, render-target binding with correct tiling/format-dependent offsets, rasterizer/scan-
converter enable, and the hyper-Z/Z-buffer state Apple's own driver treats as non-trivial) is a
substantially larger and differently-risked undertaking than anything else in this thread - closer to
the scope the original Bare-Metal R580 proposal explicitly flagged as "a different-scale project" than
a natural next increment of Stage 3's original "reproduce one shader" framing.

This is the real, concrete boundary of what this session's investigation has reached: Stage 3's
question (does the hand-encoded native instruction produce correct output) remains genuinely open, not
because of a dead end, but because answering it decisively now requires a substantially larger,
differently-scoped effort than the rest of this proposal - real vertex/rasterizer/render-target
pipeline bring-up from a PDF spec with no reference implementation for the undocumented parts, on
hardware with a documented real hang risk in adjacent register territory, and no remote recovery if
that risk materializes.
