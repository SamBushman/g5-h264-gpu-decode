# Stage 17: frame-level GPU decode prototype (1080p luma, stock driver, plain GLSL) - results

Source: `src/frame_proto.c` (earlier variants kept: `frame_proto_v0.c` quads+dense, `frame_proto_quads.c`). G5, stock kext + GL driver, 1920x1088 synthetic inter frame: all 16 qpel phases (9 shader classes), 25% of MBs with four distinct 8x8 MVs, 20% of 4x4 blocks coded. CPU reference written straight from the spec; **result compared bit-for-bit: 0 of 2,088,960 pixels differ.**

## Timing, ms per frame (luma only; deblock separate; no chroma yet)
| version | upload | MC | residual | present | end-to-end (one glFinish) |
|---|---|---|---|---|---|
| v0 quads, dense 8 MB coef texture, full-plane residual | 17.8 | 12.6 | 7.9 | - | 37.7 |
| v1 sparse atlas (1.6 MB) + blend residual | 5.3 | 12.5 | 7.7 | 1.0 | 25.5 |
| v2 copy + single-pass residual over coded blocks | 5.7 | 12.6 | 4.9 | 1.0 | 22.9 |
| **v3 point sprites + per-frame VBOs (geometry upload included)** | 5.8 | 9.6 | 3.4 | 1.0 | **18.6** |
Deblock (luma, bS<4 filter, 2 edge passes + 2 copies, **cost stand-in: not bit-exact, edge dependency chain not reproduced**): 15.9 ms at the synthetic 56% edge density -> luma incl. deblock ~33 ms. Chroma not implemented (estimate +3-5 ms).

## What the profile showed
- MC cost by phase class (ns/px): full 3, half 4, quarter 4-5, centre-j classes ~12 (36 fetches). Centre classes are 30% of pixels but ~60% of MC time in this uniform-phase synthetic stream (real streams skew to full/half-pel).
- ~0.22 ns per texture fetch + ~1.2 ns/px base; no MRT, so the prediction must be copied (~1 ms per full-frame copy).
- Geometry submission mattered more than shaders: 72,747 4x4 regions as client-array quads = 4.5 ms of driver CPU per draw (GPU only 0.63); as point sprites 0.9 ms; static VBO 0.14 ms.

## Driver quirks found (also appended to the ati-x1900-driver-quirks skill, entries 27-31)
27. GLSL helper functions taking `vec2` offsets and composed into texture coordinates miscompile (all fractional MC classes wrong, max error 217); the same math written as macros/scalars is exact. Isolation incomplete.
28. `texture2DRect(8-bit tex).r * 255.0` is not exactly integral; `floor()` of integer-boundary expressions then flips (the project's long-standing +/-1 noise). Fix: round once (`floor(x + 0.5)`) per tap-sum / full-pel fetch -> bit-exact.
29. `gl_FragCoord` is NOT per-fragment for GL_POINTS (constant, not even the centre). Use point-sprite coord replacement (`gl_TexCoord[0]`, **lower-left** origin on this driver) plus a constant varying carrying the centre.
30. Large GL_POINTS draws from client arrays silently corrupt (a contiguous band of sprites wrong; shrinks with smaller draw chunks); VBOs are exact. GL_QUADS client arrays are exact but cost ~4.5 ms CPU per 72k quads.
31. No multiple render targets (no ARB/ATI_draw_buffers) - not a bug, a limit: costs an extra copy pass.
