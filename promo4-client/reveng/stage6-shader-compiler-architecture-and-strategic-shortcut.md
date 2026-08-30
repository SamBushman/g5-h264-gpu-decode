# The shader compilation pipeline: architecture, and a real strategic shortcut for PROMO4

Closes the last of the two gaps identified as most load-bearing in the "unexplored driver surface"
survey (the IOUserClient dispatch table was the other, done in `stage5-...`). This one turned into
something more valuable than expected: not just documentation of an unexplored area, but a genuine
strategic simplification of PROMO4's whole approach to running custom shaders.

## The real pipeline, end to end

`ATIRadeonX1000GLDriver.bundle` links directly against `libGLProgrammability.dylib` and imports exactly
five real symbols: `_glpPPShaderToProgram`, `_glpFreePPShaderToProgram`, `_glpPPShaderLinearize`,
`_glpFreePPShaderLinearize`, `_glpUniformToFloat`. **It does *not* import or call any
`PPCRuntimeCompiler`/`PPCRasterOpMachine` symbol** (a separate, richly-named API family also present in
the same dylib) - a real, useful negative result: that family is Apple's software/CPU-emulation
fallback-renderer compiler (the "PPC" there is a coincidental-looking but real reference to compiling
*to actual PowerPC machine code* for CPU-side shader execution), architecturally separate from the real
GPU-hardware path this project cares about.

The real hardware path, decoded this pass (fresh Ghidra imports of both binaries, ATI bundle already
had a project; `libGLProgrammability.dylib` freshly imported from the local Tiger-HD pull):

1. **`_glpPPShaderToProgram(param_1, param_2, param_3)`** (`0x97c057a4` in this dylib's own address
   space) - the real front-end entry point. Dispatches on the real GL enum at `*param_1`:
   `0x8b30`=`GL_FRAGMENT_SHADER_ARB` or `0x8b31`=`GL_VERTEX_SHADER_ARB`, each selecting different parser
   configuration constants (`_PPParserCreate(2)` vs `_PPParserCreate(0)`), then parses the raw shader
   source text (`_PPParserAttachString`/`_PPParserParse`) into a real, generic token/operation stream
   (`_PPStreamCreate`/`_PPStreamGetStream`). This is a genuine ARB-assembly-and/or-GLSL-family text
   parser - the `T*`-prefixed classes found elsewhere in this dylib's symbol table
   (`TIntermediate`, `TParseContext`, `TIntermAggregate`, `TAllocation`) directly confirm this embeds
   the real 3Dlabs/glslang reference GLSL compiler front-end lineage.
2. **`_glpPPShaderLinearize(param_1, param_2)`** (`0x97c07c30`) - takes that generic stream and runs it
   through **`_glpPPShaderLinearizeStreamMgr`** (`0x97c07af0`), a real, complete, hardware-agnostic
   optimizing middle-end, gated by real bitflags in `param_2`. Fully decoded, and every pass is a real,
   classic compiler technique with a self-descriptive name:
   - `InlineFunctionsBranch`
   - `DetectConstantLoopsSimple` / `UnrollConstantLoopsSimple` (loop unrolling)
   - `PPStreamPackIndices` / `FlattenIfs` (control-flow flattening - GPUs of this era have no real
     branching, so `if`s get flattened to predicated/masked straight-line code)
   - `LinearizeFixOutputReads`
   - `ConstantPropagateFold` (constant folding)
   - `LocalCopyPropagator`
   - `EmbedSamplerParamIndices`
   - `ProgramNew` / `Registerify` / `Blockify` (converts the flat stream into a real basic-block/register
     form)
   - `BuildGenKill` / `BuildLiveOut` (classic liveness analysis)
   - `DeadCodeEliminationSimple`
   - `BuildInterferenceSets` / `RegistersMerge` (**a real interference-graph register allocator** -
     this is a genuine, complete register-allocation pass, not a stub)
   - `RegistersCleanup` / `ProgramFree`

   The result, retrieved via `_PPStreamGetStream` into a caller-allocated buffer sized `count * 8`
   bytes, is a flat array of **8-byte generic instruction records** - partially decoded from
   `_glpUniformToFloat`'s own record-parsing code: dword 0 packs a 5-bit type field (bits 16-20), a
   3-bit sub-type/flag field (bits 26-28), and a 16-bit index/id field (bits 0-15); dword 1 is a
   signed value (an operand index or similar). Not a complete format specification, but enough to
   recognize and walk the array.
3. **`_glpUniformToFloat`** - converts a uniform/parameter's live float value into the four-component
   form the hardware constant store expects, including a real "negate" transform (`bVar5` path) driven
   by a flag bit in the same record format above - confirms this record format also governs constant/
   uniform *usage sites* within the linearized program, not just instructions.

**What happens after this is genuinely vendor-specific, and was not located this pass.** The ATI
driver bundle's `_gldCreatePipelineProgram`/`_gldModifyPipelineProgram` (the real vendor-side program
object API, previously unexplored - see below) only set dirty flags and defer; the real final "walk the
linearized array and emit `US_ALU_RGBA_INST`-style R5xx instruction words" function was not found by
either symbol name or targeted literal search (a search for the confirmed shader-instruction literal
`0x2049`/known US-block register addresses inside the ATI bundle produced only a false-positive hit on
unrelated vtable-pointer-store stubs - a real methodology lesson: literal-value search across a whole
large binary without narrowing to plausible call context reproduces the same false-positive risk
already documented for `&DAT_xxxx` collisions in `stage4-radeon3DCopySetup-complete-draw-reference.md`).
This final step remains a genuine, unlocated gap - most likely a single, large function inside
`ATIRadeonX1000GLDriver.bundle` reachable from the lazy-compile path triggered at bind/draw time (not
traced this pass; `_gldModifyPipelineProgram`'s own "compile" call is a no-op stub, confirming
compilation is deferred past this call, not inline within it).

## `_gldCreatePipelineProgram` family, decoded (previously uncatalogued, part of the 62-symbol API)

- `_gldCreatePipelineProgram`: trivial - allocates a 0x40-byte object, stores the caller's program name.
- `_gldModifyPipelineProgram`: ORs new dirty-state bits into the object, then calls a **no-op stub**
  (`FUN_00024fd0`) - real, direct confirmation that shader compilation is lazy/deferred, not eager on
  modification.
- `_gldRelatePipelineProgram`: attaches a vertex or fragment shader sub-object to the program (checked
  via the same `0x8b30`/`0x8b31` ARB enum values `_glpPPShaderToProgram` uses) - the real `glAttachShader`-
  equivalent for this legacy pipeline-object model.
- `_gldGetPipelineProgramInfo`: real `glGetProgramivARB`-style query path (`FUN_000269a0`), including a
  real lazy per-target compiled-info cache allocation (sized from a lookup table keyed by target:
  `GL_VERTEX_PROGRAM_ARB`=`0x8620` handled inline, `GL_FRAGMENT_PROGRAM_ARB`=`0x8804` deferred to a
  helper) - but this function only manages query-result caching, not real instruction emission; the
  actual hardware compile trigger remains unlocated (see above).
- `_gldDestroyPipelineProgram`: straightforward cleanup, clears any dangling per-context binding slots.

## The real strategic implication for PROMO4

This is the valuable part. **A from-scratch PROMO4 client does not need to write or reverse-engineer a
GLSL/ARB-assembly compiler at all.** `libGLProgrammability.dylib` is a real, public, directly linkable
system framework (not a private/undocumented driver-internal component) exposing exactly the front-end
parsing and full middle-end optimization (including real register allocation) any custom shader would
need - for free, from Apple's own, already-correct, already-tested code. A future PROMO4 client can:

1. Link `libGLProgrammability.dylib` directly and call `_glpPPShaderToProgram` on ordinary ARB-assembly
   or GLSL-family shader source text, exactly as the real ATI driver bundle does.
2. Call `_glpPPShaderLinearize` to get a fully-optimized, already-register-allocated flat instruction
   array - inlining, loop unrolling, constant folding, dead-code elimination, and register allocation
   already done.
3. Write **one new, much smaller function**: a walker over that 8-byte-record array that emits the
   final R5xx `US_ALU_RGBA_INST`-style hardware words - the one piece of genuinely new code required,
   and a dramatically smaller undertaking than either hand-writing R5xx assembly per test shader or
   fully reverse-engineering the ATI driver's own unlocated final-codegen function.

This directly extends and de-risks `stage4-TOP-DOWN-promo4-redesign-proposal.md`'s existing plan (which
already assumed hand-written/AMD-doc-derived register sequences for a *fixed*, minimal test shader): any
*future*, more ambitious custom-shader work no longer needs a bespoke compiler-writing effort at all.

## Honest limits

- The exact final "generic IR -> real R5xx instruction word" encoder was not located. This is the one
  genuinely vendor-specific piece of the whole pipeline, and remains real, unlocated, uncompleted work -
  most plausibly inside `ATIRadeonX1000GLDriver.bundle`, reachable only from the lazy-compile path at
  bind/draw time, not from any function reachable via the symbols or literals checked this pass.
- The 8-byte generic instruction record format is only partially decoded (one dword's bitfield layout,
  inferred from a single consumer function) - enough to recognize the format, not enough to fully
  specify it without further tracing (a real, scoped follow-up if a future session wants to actually
  build the encoder from item 3 above).
- Nothing in this document was run against real hardware or even exercised live in a debugger - pure
  static analysis of the two locally-held binaries, per the standing constraint for this session (no G5
  access).
