# Stage 2: payload-format question resolved - real PM4, confirmed by the kext's own code

Resolves the central open question flagged in `stage2-prep-command-buffer.md` and
`stage2a-context-allocation.md`: does the mapped command-buffer payload area expect raw PM4, or an
Apple-intermediate format? Found via a raw string search of the kext binary (`ATIRadeonX1000`, already
pulled locally in `reverse-eng/`) that turned up real, directly-named C++ symbols this session hadn't
yet seen - `track_regs_written_by_pm4`, `submit_buffer`, `submit_ring_data`, `submit_empty_buffer`,
`submit_buffer_retired`, `write_r500_3d_blit_state_packet`, `process_command_buffer`, and the module
name `_aPM4_Microcode_R520` itself.

## `track_regs_written_by_pm4`: the real PM4 parser, fully decompiled

```c
void track_regs_written_by_pm4(tracked_register_set *param_1, ulong *param_2, ulong *param_3) {
  while (param_2 < param_3) {
    uVar6 = *param_2;
    uVar3 = uVar6 >> 0x1e;                    // bits 31:30 = TYPE - exact match to AMD's spec
    if (uVar3 == 1) {                         // Type-1: two packed register writes
      store_reg(param_1, uVar6 & 0x7ff, param_2[1]);
      store_reg(param_1, uVar6 >> 0xb & 0x7ff, param_2[2]);
      param_2 += 3;
    } else if (uVar3 == 0) {                  // Type-0: N consecutive (or repeated) register writes
      COUNT = uVar6 >> 0x10 & 0x3fff;         // bits 29:16 - exact match
      BASE_INDEX = uVar6 & 0x1fff;            // bits 12:0(ish) - exact match
      ONE_REG_WR = uVar6 & 0x8000;            // bit 15 - exact match
      ... writes COUNT+1 registers, either consecutive or all to BASE_INDEX ...
    } else if (uVar3 == 2) {                  // Type-2: FILLER
      param_2 = param_2 + 1;                  // skip exactly one dword, do nothing else
    } else if (uVar3 == 3) {                  // Type-3: opcode packet
      param_2 = param_2 + (COUNT-derived skip) + 8bytes;   // skip header+body, no register tracking
    }
  }
}
```

Every field position (`TYPE` at bits 31:30, Type-0's `COUNT`/`BASE_INDEX`/`ONE_REG_WR`, Type-2's
"just skip one dword") matches AMD's own official R5xx Acceleration doc (§6.1, extracted earlier this
stage) exactly, field-for-field. **This settles the question directly, not by inference**: the kernel
genuinely parses real PM4 out of client-visible buffers.

## `submit_ring_data`: the real ring-submission function, and it uses Type-2 filler itself

```c
void ATIRadeonX1000::submit_ring_data(ATIRadeonX1000 *this) {
  ...
  if ((localWriteIdx & 7) != 0) {             // pad to an 8-dword boundary before submitting
    do {
      ring[localWriteIdx] = 0x80000000;       // <-- a literal real Type-2 filler packet
      localWriteIdx = (localWriteIdx + 1) & 0x7ff;
    } while ((localWriteIdx & 7) != 0);
  }
  ... PowerPC cache management (dataCacheBlockStore/Flush, sync, instructionSynchronize) ...
  *(uint *)(hwBase + 0x714) = localWriteIdx << 0x18 | ...;   // real MMIO write to the ring
                                                               //   write-pointer register, matching
                                                               //   the 0x714 offset confirmed earlier
                                                               //   this session
  enforceInOrderExecutionIO();
  this->lastSubmittedIdx = localWriteIdx;
}
```

**This is direct, real, code-level confirmation**: `0x80000000` (TYPE=2, all other bits zero) is
exactly the value this exact driver's own real submission path writes as inert ring padding, not just
a value AMD's public documentation happens to describe as safe. The two sources agree completely.

**Real architectural detail this also reveals**: the ring this function writes to (`this+0x900` base,
`this+0x914`/`this+0x918` indices, masked `& 0x7ff` - an 11-bit index, i.e. a 2048-entry / 8KB ring) is
a separate, smaller, kernel-internal structure from the per-client 128KB command buffer Stage 2a
mapped (memory type 1). There is a translation/copy step between "client writes PM4 into its own
mapped buffer" and "kernel appends into the real hardware ring" that this pass hasn't traced in detail
(likely `ATIRadeonX1000::submit_buffer`, not yet decompiled) - but the PAYLOAD FORMAT question is
answered regardless: what a client writes is real PM4, parsed and handled as real PM4 by the kernel,
whichever intermediate copy path it takes to reach the hardware ring.

## `submit_empty_buffer`: a related but distinct mechanism, not yet fully understood

Also real and decompiled, but turns out NOT to be about the PM4 ring at all - it does a guarded,
polled register handshake (write `0x10000000`/`0x22000000` to an offset, poll a status field with up
to 10001 retries) against what looks like a different hardware block. Interesting, but not the "submit
nothing to the ring" shortcut it sounded like from its name alone - not pursued further this pass;
flagged honestly rather than assumed.

## What this changes for Stage 2's actual test

The earlier plan (`stage2-prep-command-buffer.md`) deliberately avoided guessing at payload format.
That question is now answered with real, direct evidence from two independent sources (AMD's public
spec, and this exact driver's own real code). Stage 2's actual first submission test can now proceed
with real confidence: write one real Type-2 filler packet (`0x80000000`) into the mapped command
buffer's payload area (starting at buffer offset 0x20, per the confirmed `VendorCommandBufferHeader`
layout), then trigger the real re-map/retire cycle Stage 2-prep already confirmed happens kernel-side,
and observe the result.
