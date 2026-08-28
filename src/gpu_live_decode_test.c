/*
 * gpu-live-decode-test: closes the "real integration" gap. The x1900
 * mb hook now actually RECONSTRUCTS real intra16x16 macroblocks (luma
 * via CPU intra16x16 prediction + GPU IDCT + GPU luma-DC-Hadamard, all
 * already verified by gpu-full-intra16-test; chroma via CPU intra8x8
 * chroma prediction + GPU IDCT + a CPU chroma-DC 2x2 Hadamard transform,
 * new this test) and WRITES the result into the live frame buffer,
 * returning 1 to skip FFmpeg's own CPU reconstruction for that
 * macroblock entirely. Anything not intra16x16 (I4x4, inter/P/B) still
 * declines (returns 0) and falls back to FFmpeg's normal software path,
 * matching the project's original "decline and let software handle it"
 * design - this is a real, live, partially-GPU-assisted decode, not an
 * all-or-nothing switch.
 *
 * Verification: decode the same file twice - once with the hook
 * installed and live (return 1 for qualifying MBs), once completely
 * normally (no hook) - and diff the two decoded frames pixel-for-pixel.
 * Both decodes still run FFmpeg's own deblocking filter identically
 * (deblocking is a separate pass over whatever pixels are in the
 * buffer, oblivious to whether the hook or FFmpeg's C code put them
 * there), so a correct hook should produce frames that are bit-exact
 * (or within ordinary decode noise) against the true CPU-only decode -
 * a much stronger, harder-to-fool test than the previous milestone's
 * single-macroblock-in-isolation comparisons.
 */

#include "mp4box.h"
#include <AGL/agl.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/x1900_hook.h>

#define MB_TYPE_INTRA4x4   (1 << 0)
#define MB_TYPE_INTRA16x16 (1 << 1)
#define MB_TYPE_INTRA_PCM  (1 << 2)
#define MB_TYPE_16x16      (1 << 3) /* mpegutils.h - whole-macroblock single
                               * MV/ref partition (as opposed to 16x8/8x16/
                               * 8x8 sub-partitions, not yet implemented -
                               * see the plan's "Sub-8x8 partitions" item).
                               * Set for BOTH true P_16x16-with-residual and
                               * (per FFmpeg's own mb_type assignment) P_Skip
                               * macroblocks - MB_TYPE_SKIP is checked first
                               * in live_hook's dispatch, so this bit alone
                               * is only reached for the non-skip case. */
#define IS_INTRA_MB(t)     ((t) & (MB_TYPE_INTRA4x4 | MB_TYPE_INTRA16x16 | MB_TYPE_INTRA_PCM))
#define MB_TYPE_16x8       (1 << 4) /* mpegutils.h - two 16x8 partitions
                               * (top/bottom halves), each its own MV -
                               * see the plan's "Item 3 (sub-8x8
                               * partitions)" design pass. */
#define MB_TYPE_8x16       (1 << 5) /* mpegutils.h - two 8x16 partitions
                               * (left/right halves), each its own MV. */
#define MB_TYPE_8x8        (1 << 6) /* mpegutils.h - four 8x8 quadrants, each
                               * with its own sub_mb_type deciding further
                               * splitting (whole-8x8/8x4/4x8/4x4) - see
                               * IS_SUB_8X8/8X4/4X8/4X4 below and the plan's
                               * item-3 design pass (phase 3b: whole-
                               * quadrant; phase 3c: the three finer
                               * shapes, both now implemented). */
#define IS_SUB_8X8(a)      ((a) & MB_TYPE_16x16) /* real gotcha, verified
                               * from h264dec.h:96-99 ("note reused" in
                               * FFmpeg's own source comment): sub_mb_type's
                               * own shape bits REUSE mb_type's
                               * MB_TYPE_16x16/16x8/8x16/8x8 bit values -
                               * these macros only make sense applied to a
                               * sub_mb_type entry, never to mb_type itself. */
#define IS_SUB_8X4(a)      ((a) & MB_TYPE_16x8)  /* reuses MB_TYPE_16x8's bit */
#define IS_SUB_4X8(a)      ((a) & MB_TYPE_8x16)  /* reuses MB_TYPE_8x16's bit */
#define IS_SUB_4X4(a)      ((a) & MB_TYPE_8x8)   /* reuses MB_TYPE_8x8's bit */
#define MB_TYPE_8x8DCT     (1 << 24) /* h264_parse.h "MB_TYPE_CODEC_SPECIFIC" -
                               * real, separate H.264 PPS feature
                               * (transform_8x8_mode): residual for this
                               * macroblock uses an 8x8 IDCT
                               * (h264_idct8_add4) instead of the 4x4-per-
                               * block IDCT (h264_idct_add16) this
                               * project's gpu_idct_batch assumes. A real
                               * bug found implementing item 3: this
                               * content DOES use 8x8 transform mode on a
                               * meaningful fraction of macroblocks
                               * (confirmed via MB(37,1) at TARGET_FRAME=1)
                               * - reconstructing with the wrong-size IDCT
                               * produced widespread, real (not rounding-
                               * noise-scale) luma errors once P_16x8/
                               * P_8x16 started taking over enough
                               * macroblocks to make it visible. Must
                               * decline whenever this bit is set - GPU
                               * 8x8 IDCT support is real future work, not
                               * attempted here. */

/* Diagnostic only - correlate hook activity with real decode order across
 * frames. Deliberately the SAME variable h264_mb.c's own ground-truth debug
 * trace increments (extern, defined there) rather than a separate local
 * counter - two independently-incrementing counters (one per file) would
 * drift out of sync the moment this test harness's own decode_to_frame()
 * resets ITS counters per call but h264_mb.c's process-lifetime counter
 * doesn't also get reset, making frame numbers from the two traces
 * incomparable (this bit a real debugging session directly - see the
 * "P_16x16-with-residual" investigation notes). */
extern int g_x1900_debug_frameno;
/* The DECODE-order frameno (see g_x1900_debug_frameno) that corresponds to
 * the DISPLAY-order target_frame this run actually captured into
 * live/ref - set by decode_to_frame() the moment it finds that frame.
 * Needed because target_frame is a display-order index and this clip has
 * B-frames, so decode order and display order genuinely diverge. */
static int g_x1900_debug_captured_frameno = -1;

/* Item 10 follow-up: coarse GPU-dispatch profiling, gated by
 * DEBUG_GPU_PROFILE - times each real glFinish()+glReadPixels()
 * round-trip site, since the design's standing hypothesis ("per-flush
 * sync dominates") had never actually been measured broken down by
 * site. Reset once per decode_multi()/decode_to_frame() call, read by
 * main() right after, same pattern as g_took_over etc. */
static double g_prof_lumadc_ms = 0, g_prof_idct_ms = 0, g_prof_mc_ms = 0;
static int g_prof_lumadc_n = 0, g_prof_idct_n = 0, g_prof_mc_n = 0, g_prof_flush_n = 0;
static int g_prof_enabled = -1; /* -1 = not yet checked */
static int prof_on(void) {
    if (g_prof_enabled < 0) g_prof_enabled = getenv("DEBUG_GPU_PROFILE") ? 1 : 0;
    return g_prof_enabled;
}
static double prof_ms(struct timeval *a, struct timeval *b) {
    return (b->tv_sec - a->tv_sec) * 1000.0 + (b->tv_usec - a->tv_usec) / 1000.0;
}

/* Item 9 investigation (2026-08-28): a synthetic probe (finish_probe.c)
 * showed glFinish() itself genuinely yields (2.8% CPU during an 84ms wait)
 * but glReadPixels alone costs real CPU (74% CPU during its own wall time -
 * driver-side pixel marshalling, not a wait). These CPU-time accumulators
 * (getrusage-based, alongside the existing wall-time ones) find out how
 * much of THIS project's real per-site cost is genuine GPU-wait (CPU free)
 * vs. real CPU-consumed work (readback/upload marshalling) - the actual
 * question for the new "free up the CPU" goal, not just "is it slow." */
static double g_prof_lumadc_cpu_ms = 0, g_prof_idct_cpu_ms = 0, g_prof_mc_cpu_ms = 0;
/* Finer breakdown, singlepass MC dispatch only (the largest single cost) -
 * which PHASE of one dispatch is actually CPU-expensive: the CPU-side
 * packing loop (pure C, no GL), the texture uploads (glTexImage2D), the
 * draw+glFinish (GPU wait, expected cheap per the finish_probe.c finding),
 * or the readback+unpack (glReadPixels, expected expensive per that same
 * finding). Answers "what to actually fix next," not just "how much." */
static double g_prof_sp_pack_ms = 0, g_prof_sp_pack_cpu_ms = 0;
static double g_prof_sp_upload_ms = 0, g_prof_sp_upload_cpu_ms = 0;
static double g_prof_sp_draw_ms = 0, g_prof_sp_draw_cpu_ms = 0;
static double g_prof_sp_read_ms = 0, g_prof_sp_read_cpu_ms = 0;
static int g_prof_sp_chunks = 0; /* mirrors g_prof_diag_chunks - added
    2026-08-28 to check whether this family has the same "unnecessary
    chunking" headroom the diag family did before its own chunk cap was
    raised, or whether its cost is genuinely elsewhere. */
/* Same 4-way breakdown for gpu_idct_batch, now the largest single site
 * (802ms/40-frame run) after the reftex fix. */
static double g_prof_ib_pack_ms = 0, g_prof_ib_pack_cpu_ms = 0;
static double g_prof_ib_upload_ms = 0, g_prof_ib_upload_cpu_ms = 0;
static double g_prof_ib_draw_ms = 0, g_prof_ib_draw_cpu_ms = 0;
static double g_prof_ib_read_ms = 0, g_prof_ib_read_cpu_ms = 0;
static double cpu_ms(struct rusage *a, struct rusage *b) {
    double au = a->ru_utime.tv_sec * 1000.0 + a->ru_utime.tv_usec / 1000.0;
    double as_ = a->ru_stime.tv_sec * 1000.0 + a->ru_stime.tv_usec / 1000.0;
    double bu = b->ru_utime.tv_sec * 1000.0 + b->ru_utime.tv_usec / 1000.0;
    double bs = b->ru_stime.tv_sec * 1000.0 + b->ru_stime.tv_usec / 1000.0;
    return (bu - au) + (bs - as_);
}
#define MB_TYPE_SKIP       (1 << 17) /* mpegutils.h - deliberately re-declared
                               * here rather than pulled in, matching this
                               * header's own no-H264-internal-types design. */
#define MB_TYPE_L1         ((1 << 14) | (1 << 15)) /* mpegutils.h P0L1|P1L1 -
                               * set whenever a macroblock uses reference
                               * list 1 (B-slice bi-prediction, or a
                               * list-1-only macroblock). This project's
                               * whole declared scope is single-reference
                               * list-0 only (see x1900_hook.h's original
                               * mv_l0 comment: "list 1 not exposed yet") -
                               * reconstruct_skip only implements list-0
                               * motion comp, so any macroblock using list 1
                               * too must decline, not silently reconstruct
                               * using half the real prediction. Found via a
                               * real bug: MB_TYPE_SKIP alone doesn't imply
                               * single-reference - B-slice bi-predictive
                               * skip macroblocks set it too, confirmed via
                               * a direct trace of FFmpeg's own mc_dir_part
                               * showing list=1 calls for a "skip" MB whose
                               * list-0-only reconstruction didn't match. */

/* ================= GPU shaders (identical to gpu-full-intra16-test,
 * both already verified against real captured data) ================= */

static void checkgl(const char *w) { GLenum e = glGetError(); if (e) fprintf(stderr, "GL err %s: 0x%lx\n", w, (unsigned long)e); }
static GLhandleARB compile(GLenum t, const char *s) {
    GLhandleARB h = glCreateShaderObjectARB(t);
    glShaderSourceARB(h, 1, &s, NULL); glCompileShaderARB(h);
    GLint ok = 0; glGetObjectParameterivARB(h, GL_OBJECT_COMPILE_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(h, sizeof log, &n, log); fprintf(stderr, "compile fail:\n%s\n", log); exit(1); }
    return h;
}
static GLhandleARB linkp(const char *vs, const char *fs) {
    GLhandleARB p = glCreateProgramObjectARB();
    glAttachObjectARB(p, compile(GL_VERTEX_SHADER_ARB, vs));
    glAttachObjectARB(p, compile(GL_FRAGMENT_SHADER_ARB, fs));
    glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok);
    if (!ok) { char log[4096]; GLsizei n; glGetInfoLogARB(p, sizeof log, &n, log); fprintf(stderr, "link fail:\n%s\n", log); exit(1); }
    return p;
}
static const char *vs_plain = "void main(){gl_Position=gl_ModelViewProjectionMatrix*gl_Vertex;}";

/* Batched luma-DC Hadamard (item 10 follow-up, 2026-08-28) - the ONLY
 * lumadc shader this test uses now (an earlier one-macroblock-per-draw
 * version, proven bit-identical, was removed once this batched form proved
 * the same - see gpu_lumadc_batch's own history comment). Generalized the
 * same way fs_idct_batch generalizes the
 * single-block IDCT shader - N independent 16-DC-value blocks packed side by
 * side (block b in columns [4b,4b+4)), one draw call + one readback instead
 * of N. Real per-fragment difference from fs_idct_batch: qmul can vary PER
 * MACROBLOCK (real per-MB delta QP), not shared across the whole batch like
 * the single-block shader's uniform assumed - looked up per-block from a
 * second N-wide, 1-tall texture (qmulTex), same "small per-column lookup
 * texture" pattern item 4's colInfoTex already established for MC. The
 * block-base offset is plain floor() arithmetic on gl_FragCoord/qmulTex
 * lookup by computed (not branched) coordinate, so this stays clear of
 * quirk #2 (texture2DRect inside a runtime branch voids the whole draw). */
static const char *fs_lumadc_batch =
"uniform sampler2DRect dcTex;\n"
"uniform sampler2DRect qmulTex;\n"
"void main() {\n"
"  vec2 p = floor(gl_FragCoord.xy);\n"
"  float base = floor(p.x / 4.0) * 4.0;\n"
"  float blockIdx = floor(p.x / 4.0);\n"
"  float qmul = texture2DRect(qmulTex, vec2(blockIdx+0.5, 0.5)).r;\n"
"  float c0 =texture2DRect(dcTex,vec2(base+0.5,0.5)).r; float c1 =texture2DRect(dcTex,vec2(base+1.5,0.5)).r;\n"
"  float c2 =texture2DRect(dcTex,vec2(base+2.5,0.5)).r; float c3 =texture2DRect(dcTex,vec2(base+3.5,0.5)).r;\n"
"  float c4 =texture2DRect(dcTex,vec2(base+0.5,1.5)).r; float c5 =texture2DRect(dcTex,vec2(base+1.5,1.5)).r;\n"
"  float c6 =texture2DRect(dcTex,vec2(base+2.5,1.5)).r; float c7 =texture2DRect(dcTex,vec2(base+3.5,1.5)).r;\n"
"  float c8 =texture2DRect(dcTex,vec2(base+0.5,2.5)).r; float c9 =texture2DRect(dcTex,vec2(base+1.5,2.5)).r;\n"
"  float c10=texture2DRect(dcTex,vec2(base+2.5,2.5)).r; float c11=texture2DRect(dcTex,vec2(base+3.5,2.5)).r;\n"
"  float c12=texture2DRect(dcTex,vec2(base+0.5,3.5)).r; float c13=texture2DRect(dcTex,vec2(base+1.5,3.5)).r;\n"
"  float c14=texture2DRect(dcTex,vec2(base+2.5,3.5)).r; float c15=texture2DRect(dcTex,vec2(base+3.5,3.5)).r;\n"
"  float z0,z1,z2,z3;\n"
"  float m0,m1,m2,m3,m4,m5,m6,m7,m8,m9,m10,m11,m12,m13,m14,m15;\n"
"  z0=c0+c1;  z1=c0-c1;  z2=c2-c3;  z3=c2+c3;  m0=z0+z3; m1=z0-z3; m2=z1-z2; m3=z1+z2;\n"
"  z0=c4+c5;  z1=c4-c5;  z2=c6-c7;  z3=c6+c7;  m4=z0+z3; m5=z0-z3; m6=z1-z2; m7=z1+z2;\n"
"  z0=c8+c9;  z1=c8-c9;  z2=c10-c11;z3=c10+c11;m8=z0+z3; m9=z0-z3; m10=z1-z2;m11=z1+z2;\n"
"  z0=c12+c13;z1=c12-c13;z2=c14-c15;z3=c14+c15;m12=z0+z3;m13=z0-z3;m14=z1-z2;m15=z1+z2;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m8;  z1=m0-m8;  z2=m4-m12; z3=m4+m12;\n"
"  o0=floor((z0+z3)*qmul/256.0+0.5); o4=floor((z1+z2)*qmul/256.0+0.5); o8=floor((z1-z2)*qmul/256.0+0.5); o12=floor((z0-z3)*qmul/256.0+0.5);\n"
"  z0=m1+m9;  z1=m1-m9;  z2=m5-m13; z3=m5+m13;\n"
"  o1=floor((z0+z3)*qmul/256.0+0.5); o5=floor((z1+z2)*qmul/256.0+0.5); o9=floor((z1-z2)*qmul/256.0+0.5); o13=floor((z0-z3)*qmul/256.0+0.5);\n"
"  z0=m2+m10; z1=m2-m10; z2=m6-m14; z3=m6+m14;\n"
"  o2=floor((z0+z3)*qmul/256.0+0.5); o6=floor((z1+z2)*qmul/256.0+0.5); o10=floor((z1-z2)*qmul/256.0+0.5); o14=floor((z0-z3)*qmul/256.0+0.5);\n"
"  z0=m3+m11; z1=m3-m11; z2=m7-m15; z3=m7+m15;\n"
"  o3=floor((z0+z3)*qmul/256.0+0.5); o7=floor((z1+z2)*qmul/256.0+0.5); o11=floor((z1-z2)*qmul/256.0+0.5); o15=floor((z0-z3)*qmul/256.0+0.5);\n"
"  float lc = p.x - base;\n"
"  int idx = int(lc) + int(p.y) * 4;\n"
"  float result = o0;\n"
"  if (idx==1) result=o1; else if (idx==2) result=o2; else if (idx==3) result=o3;\n"
"  else if (idx==4) result=o4; else if (idx==5) result=o5; else if (idx==6) result=o6; else if (idx==7) result=o7;\n"
"  else if (idx==8) result=o8; else if (idx==9) result=o9; else if (idx==10) result=o10; else if (idx==11) result=o11;\n"
"  else if (idx==12) result=o12; else if (idx==13) result=o13; else if (idx==14) result=o14; else if (idx==15) result=o15;\n"
"  float biased = result + 32768.0;\n"
"  float hi = floor(biased / 256.0);\n"
"  float lo = biased - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

/* Batched IDCT - the only IDCT shader this test uses (an earlier
 * one-block-per-draw version was removed once this batched form proved
 * bit-identical, see reconstruct_enqueue's history). N blocks are
 * packed side by side in one wide texture (block b occupies columns
 * [4b, 4b+4), all 4 rows) so ALL of a macroblock's 4x4 blocks (16 luma
 * + 4 Cb + 4 Cr = 24) can be transformed in a single draw call + single
 * readback, instead of 24 separate glGenTextures/upload/draw/
 * glReadPixels round trips - each fragment still redundantly computes
 * its own block's full transform (same self-select-via-position
 * pattern as the original single-block shader, just with a
 * dynamically-computed per-fragment block base instead of a fixed
 * one), so per-fragment cost is unchanged - only the number of GPU
 * round trips per macroblock drops. The block-base offset is plain
 * floor() arithmetic on gl_FragCoord, not a conditional texture fetch,
 * so this stays clear of quirk #2
 * (texture2DRect inside a branch voids the whole draw).
 *
 * Real bug found+fixed while adding P_16x16-with-residual: the output
 * used to be encoded as a SINGLE 8-bit channel (`result/64.0 + 0.5`,
 * decoded as `((r8/255)-0.5)*64`), which can only represent a final
 * residual value in [-32, +32] before the fixed-function GL_UNSIGNED_BYTE
 * readback silently clamps it - any real residual outside that range got
 * corrupted, with no GL error and no visible sign anything was wrong,
 * exactly the "silent" failure class every entry in the driver-quirks
 * catalog warns about. This went undetected through all of M6-M9 and the
 * I16x16/P_Skip integration work because every block tested there
 * genuinely happened to produce residuals within ±32 - not because the
 * encoding was actually wide enough. P_16x16-with-residual's real inter
 * coefficients (larger, more numerous AC terms than this project's prior
 * test content) were the first to actually exceed it, producing a
 * "correct-looking but clamped" result: no GL error, plausible-looking
 * pixels, but real per-pixel errors up to ~100+ that grew disproportionately
 * near whichever corner of a 4x4 block carried the block's largest true
 * residual value - easy to mistake for a motion-compensation or precision
 * bug (both were investigated and ruled out by hand-verifying the exact-
 * integer expected residual against FFmpeg's own ff_h264_idct_add formula
 * before finding this). Fixed by switching to the same two-byte R+G
 * integer encoding gpu_lumadc already uses (`biased=result+32768`,
 * `hi=floor(biased/256)`, `lo=biased-hi*256`) - a full ±32768 range, safely
 * covering any real per-pixel residual. */
/* Coefficient texture switched 2026-08-28 from GL_RGBA_FLOAT32_ATI (4
 * floats/texel, only .r ever used) to GL_LUMINANCE_FLOAT32_ATI + a fixed
 * signed-bias encoding (1 float/texel, quarter the upload payload) - the
 * same trick that gave reftex_lookup_or_upload a real 28x speedup (item 9
 * investigation), previously tried here too and REVERTED after a severe
 * regression (GL_LUMINANCE_FLOAT32_ATI doesn't preserve negative values
 * correctly on this driver, and DCT coefficients are routinely negative,
 * unlike reftex's non-negative pixel data). lum-float-signed-probe (new,
 * standalone) confirmed the bug is specifically about negative VALUES:
 * biasing every value to be non-negative before upload (+65536.0, chosen
 * for wide margin over any real dequantized coefficient - exact-integer
 * float32 precision holds losslessly to 2^23 per precision-boundary-probe,
 * so this costs nothing) round-tripped 150/150 test values exactly,
 * matching the GL_RGBA_FLOAT32_ATI control. Each texture2DRect read below
 * subtracts the same bias back out immediately, before any further math
 * touches the value - numerically identical to the unbiased version from
 * that point on, including the floor()-based intermediate rounding steps
 * further down (bias cancels exactly, not an approximation). */
static const char *fs_idct_batch =
"uniform sampler2DRect coeffTex;\n"
"void main() {\n"
"  vec2 p = floor(gl_FragCoord.xy);\n"
"  float base = floor(p.x / 4.0) * 4.0;\n"
"  float lc = p.x - base;\n"
"  float lr = p.y;\n"
"  float c0  = texture2DRect(coeffTex, vec2(base+0.5, 0.5)).r - 65536.0;\n"
"  float c1  = texture2DRect(coeffTex, vec2(base+1.5, 0.5)).r - 65536.0;\n"
"  float c2  = texture2DRect(coeffTex, vec2(base+2.5, 0.5)).r - 65536.0;\n"
"  float c3  = texture2DRect(coeffTex, vec2(base+3.5, 0.5)).r - 65536.0;\n"
"  float c4  = texture2DRect(coeffTex, vec2(base+0.5, 1.5)).r - 65536.0;\n"
"  float c5  = texture2DRect(coeffTex, vec2(base+1.5, 1.5)).r - 65536.0;\n"
"  float c6  = texture2DRect(coeffTex, vec2(base+2.5, 1.5)).r - 65536.0;\n"
"  float c7  = texture2DRect(coeffTex, vec2(base+3.5, 1.5)).r - 65536.0;\n"
"  float c8  = texture2DRect(coeffTex, vec2(base+0.5, 2.5)).r - 65536.0;\n"
"  float c9  = texture2DRect(coeffTex, vec2(base+1.5, 2.5)).r - 65536.0;\n"
"  float c10 = texture2DRect(coeffTex, vec2(base+2.5, 2.5)).r - 65536.0;\n"
"  float c11 = texture2DRect(coeffTex, vec2(base+3.5, 2.5)).r - 65536.0;\n"
"  float c12 = texture2DRect(coeffTex, vec2(base+0.5, 3.5)).r - 65536.0;\n"
"  float c13 = texture2DRect(coeffTex, vec2(base+1.5, 3.5)).r - 65536.0;\n"
"  float c14 = texture2DRect(coeffTex, vec2(base+2.5, 3.5)).r - 65536.0;\n"
"  float c15 = texture2DRect(coeffTex, vec2(base+3.5, 3.5)).r - 65536.0;\n"
"  float z0, z1, z2, z3;\n"
"  float m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13, m14, m15;\n"
"  z0 = c0 + c8;  z1 = c0 - c8;  z2 = floor(c4/2.0) - c12; z3 = c4 + floor(c12/2.0);\n"
"  m0 = z0+z3; m4 = z1+z2; m8 = z1-z2; m12 = z0-z3;\n"
"  z0 = c1 + c9;  z1 = c1 - c9;  z2 = floor(c5/2.0) - c13; z3 = c5 + floor(c13/2.0);\n"
"  m1 = z0+z3; m5 = z1+z2; m9 = z1-z2; m13 = z0-z3;\n"
"  z0 = c2 + c10; z1 = c2 - c10; z2 = floor(c6/2.0) - c14; z3 = c6 + floor(c14/2.0);\n"
"  m2 = z0+z3; m6 = z1+z2; m10 = z1-z2; m14 = z0-z3;\n"
"  z0 = c3 + c11; z1 = c3 - c11; z2 = floor(c7/2.0) - c15; z3 = c7 + floor(c15/2.0);\n"
"  m3 = z0+z3; m7 = z1+z2; m11 = z1-z2; m15 = z0-z3;\n"
"  float o0,o1,o2,o3,o4,o5,o6,o7,o8,o9,o10,o11,o12,o13,o14,o15;\n"
"  z0=m0+m2;  z1=m0-m2;  z2=floor(m1/2.0)-m3;   z3=m1+floor(m3/2.0);\n"
"  o0=floor((z0+z3)/64.0); o4=floor((z1+z2)/64.0); o8=floor((z1-z2)/64.0); o12=floor((z0-z3)/64.0);\n"
"  z0=m4+m6;  z1=m4-m6;  z2=floor(m5/2.0)-m7;   z3=m5+floor(m7/2.0);\n"
"  o1=floor((z0+z3)/64.0); o5=floor((z1+z2)/64.0); o9=floor((z1-z2)/64.0); o13=floor((z0-z3)/64.0);\n"
"  z0=m8+m10; z1=m8-m10; z2=floor(m9/2.0)-m11;  z3=m9+floor(m11/2.0);\n"
"  o2=floor((z0+z3)/64.0); o6=floor((z1+z2)/64.0); o10=floor((z1-z2)/64.0); o14=floor((z0-z3)/64.0);\n"
"  z0=m12+m14;z1=m12-m14;z2=floor(m13/2.0)-m15; z3=m13+floor(m15/2.0);\n"
"  o3=floor((z0+z3)/64.0); o7=floor((z1+z2)/64.0); o11=floor((z1-z2)/64.0); o15=floor((z0-z3)/64.0);\n"
"  int idx = int(lc) + int(lr) * 4;\n"
"  float result = o0;\n"
"  if (idx == 1) result = o1; else if (idx == 2) result = o2; else if (idx == 3) result = o3;\n"
"  else if (idx == 4) result = o4; else if (idx == 5) result = o5; else if (idx == 6) result = o6; else if (idx == 7) result = o7;\n"
"  else if (idx == 8) result = o8; else if (idx == 9) result = o9; else if (idx == 10) result = o10; else if (idx == 11) result = o11;\n"
"  else if (idx == 12) result = o12; else if (idx == 13) result = o13; else if (idx == 14) result = o14; else if (idx == 15) result = o15;\n"
"  float biased = result + 32768.0;\n"
"  float hi = floor(biased / 256.0);\n"
"  float lo = biased - hi * 256.0;\n"
"  gl_FragColor = vec4(hi/255.0, lo/255.0, 0.0, 1.0);\n"
"}\n";

static AGLContext g_glctx;

/* Item 10 follow-up (2026-08-28): batched across up to a whole row's worth
 * of intra macroblocks, replacing the original one-macroblock-at-a-time
 * gpu_lumadc() (proven correct, but a real, measured 175ms/266-call cost on
 * a 10-frame run - literally the ONLY GPU dispatch site in this project that
 * had never been folded into the per-row batching mechanism gpu_idct_batch
 * already proved out). Same n-blocks-side-by-side layout as gpu_idct_batch;
 * the one real generalization needed is qmul, which (unlike the single-MB
 * caller's shared uniform) can differ per macroblock via real delta QP - a
 * second n-wide/1-tall lookup texture, mirroring item 4's colInfoTex. */
/* Must match PENDING_MAX (defined later - see its own item-9 comment on
 * why it's now a whole-frame bound, not one row). Not literally
 * PENDING_MAX here since that's defined further down the file, after this
 * function (both gpu_idct_batch/IDCT_BATCH_MAX above have this same
 * "defined before PENDING_MAX exists" constraint). In practice this stays
 * tiny regardless (the left/top-neighbor flush check forces a flush
 * before every intra macroblock, so real batches rarely exceed 1) - sized
 * generously anyway since it's cheap. */
#define LUMADC_BATCH_MAX 1024
static void gpu_lumadc_batch(int dc16[][16], int qmul[], int n, int out[][16]) {
    struct timeval _p0, _p1; struct rusage _r0, _r1;
    if (prof_on()) { gettimeofday(&_p0, NULL); getrusage(RUSAGE_SELF, &_r0); }
    int w = n * 4;
    glViewport(0, 0, w, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, w, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    static float rgba[LUMADC_BATCH_MAX * 4 * 4 * 4];
    for (int b = 0; b < n; b++)
        for (int i = 0; i < 16; i++) {
            int row = i / 4, col = i % 4;
            int texel = row * w + (b * 4 + col);
            rgba[texel*4] = (float)dc16[b][i]; rgba[texel*4+1] = rgba[texel*4+2] = 0; rgba[texel*4+3] = 1;
        }
    static GLuint tex = 0;
    if (!tex) {
        glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    } else {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    }
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,w,4,0,GL_RGBA,GL_FLOAT,rgba);

    static float qrgba[LUMADC_BATCH_MAX * 4];
    for (int b = 0; b < n; b++) { qrgba[b*4] = (float)qmul[b]; qrgba[b*4+1] = qrgba[b*4+2] = 0; qrgba[b*4+3] = 1; }
    static GLuint qtex = 0;
    if (!qtex) {
        glGenTextures(1,&qtex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,qtex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    } else {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB,qtex);
    }
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_RGBA_FLOAT32_ATI,n,1,0,GL_RGBA,GL_FLOAT,qrgba);

    static GLhandleARB prog = 0;
    static GLint loc_dcTex = -1, loc_qmulTex = -1;
    if (!prog) {
        prog = linkp(vs_plain, fs_lumadc_batch);
        /* Item 9 investigation: caching these once instead of looking them
         * up by string name every dispatch call - real, measured CPU cost
         * (draw+finish was 92% CPU for MC singlepass, not genuine GPU wait;
         * per-call uniform-location string lookups are part of that "extra
         * CPU work per small draw call" driver overhead). */
        loc_dcTex = glGetUniformLocationARB(prog, "dcTex");
        loc_qmulTex = glGetUniformLocationARB(prog, "qmulTex");
    }
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(loc_dcTex,0);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,qtex);
    glUniform1iARB(loc_qmulTex,1);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(w,0);glVertex2f(w,4);glVertex2f(0,4); glEnd();
    /* Item 9 follow-up (2026-08-28): no glFinish() here - a real, measured
     * win on a synthetic probe (fence_pipeline_probe.c, "variant B": 30-65%
     * less wall time, same/lower CPU, across repeated runs) for exactly
     * this shape - a glFinish() immediately followed by a glReadPixels()
     * that already blocks on the same data. The explicit stall was pure
     * overhead beyond what the readback's own implicit sync provides;
     * removing it is safe on this driver (same conclusion the diag stage1
     * ->stage2 glFinish() removal reached earlier this session, now
     * confirmed to generalize to a draw->readback boundary, not just a
     * draw->draw one). checkgl() stays - it's a cheap client-side
     * glGetError() poll, not a synchronization point. */
    checkgl("lumadc batch draw");
    static unsigned char px[LUMADC_BATCH_MAX * 4 * 4 * 4];
    glReadPixels(0,0,w,4,GL_RGBA,GL_UNSIGNED_BYTE,px);
    for (int b = 0; b < n; b++)
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++) {
                int texel = row * w + (b * 4 + col);
                int hi = px[texel*4], lo = px[texel*4+1];
                out[b][row*4+col] = hi*256 + lo - 32768;
            }
    if (prof_on()) { gettimeofday(&_p1, NULL); getrusage(RUSAGE_SELF, &_r1);
        g_prof_lumadc_ms += prof_ms(&_p0, &_p1); g_prof_lumadc_cpu_ms += cpu_ms(&_r0, &_r1); g_prof_lumadc_n++; }
}

/* Cross-macroblock batching: a whole row's worth of qualifying
 * macroblocks (up to 40 for this project's 640-wide test content) can
 * be queued and IDCT'd in one mega-batch - see the PendingMB queue
 * below. 40 macroblocks * 24 blocks/MB * 4 texels/block = 3840 texels
 * wide, safely under this driver's GL_MAX_TEXTURE_SIZE=4096 (M4 probe). */
#define IDCT_BATCH_MAX (40 * 24)
/* Signed-bias added to every coefficient before uploading to the
 * GL_LUMINANCE_FLOAT32_ATI coefficient texture (gpu_idct_batch) so no
 * value going in is ever negative - see that function's own comment for
 * why. Generous margin over any real dequantized H.264 coefficient this
 * project has ever seen; float32 holds integers exactly up to 2^23
 * (precision-boundary-probe), so this adds zero precision cost. */
#define IDCT_COEFF_BIAS 65536.0f

/* Batched IDCT: transforms `n` independent 4x4 blocks in one draw call
 * + one readback instead of `n` separate round trips.
 * coeffs16/out are [n][16], laid out exactly like gpu_idct4x4's own
 * per-block array. See fs_idct_batch for the packing scheme.
 * rgba/px are static (not stack) both to avoid a ~250KB/~60KB stack
 * allocation every call and because they're too large to comfortably
 * put on the stack at all once n can span a whole row of macroblocks.
 *
 * Item 9 frame-scale restructure (2026-08-28): `n` can now span up to a
 * whole frame's worth of blocks (up to PENDING_MAX*24, since flushing is
 * no longer bounded to one row), which would blow well past
 * GL_MAX_TEXTURE_SIZE=4096 in a single draw call. IDCT_BATCH_MAX now
 * means "blocks per real GL round trip" (unchanged value, 960 - still
 * safely under the 4096 texel-width limit at 4 texels/block), and this
 * function chunks internally: multiple draw+readback round trips if
 * n > IDCT_BATCH_MAX, transparent to the caller (which still just passes
 * a big flat coeffs16/out array sized for the whole batch). MC's own
 * dispatch functions already had this chunking (item 4/quirk #16); this
 * is the same pattern applied here. */
static void gpu_idct_batch(int coeffs16[][16], int n, int out[][16]) {
    for (int base = 0; base < n; base += IDCT_BATCH_MAX) {
    int chunk_n = (n - base < IDCT_BATCH_MAX) ? (n - base) : IDCT_BATCH_MAX;
    struct timeval _p0, _p1; struct rusage _r0, _r1;
    if (prof_on()) { gettimeofday(&_p0, NULL); getrusage(RUSAGE_SELF, &_r0); }
    int w = chunk_n * 4;
    glViewport(0, 0, w, 4);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, w, 0, 4, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    /* Item 9 fix ATTEMPTED then REVERTED (2026-08-28), then RECLAIMED
     * 2026-08-28 (same day, later session): GL_LUMINANCE_FLOAT32_ATI (1
     * float/texel vs GL_RGBA_FLOAT32_ATI's 4, only .r was ever read)
     * quarters the upload payload, matching reftex_lookup_or_upload's real
     * 28x win - but a first attempt caused a severe regression (3.9-5.3%
     * mismatch), traced to GL_LUMINANCE_FLOAT32_ATI not preserving
     * negative values on this driver (DCT coefficients are routinely
     * negative, unlike reftex's non-negative pixel data). Reverted then;
     * the flagged follow-up (a signed-bias encoding) was built and
     * verified standalone (lum-float-signed-probe, 150/150 exact
     * round-trips across a wide signed-value sweep, matching a
     * GL_RGBA_FLOAT32_ATI control) before landing here. See
     * fs_idct_batch's own comment above for the exact bias value and why
     * it costs nothing precision-wise. */
    static float lum[IDCT_BATCH_MAX * 4 * 4]; /* width(chunk_n*4) * height(4) * 1 float/texel */
    for (int b = 0; b < chunk_n; b++)
        for (int i = 0; i < 16; i++) {
            int row = i / 4, col = i % 4;
            int texel = row * w + (b * 4 + col);
            lum[texel] = (float)coeffs16[base + b][i] + IDCT_COEFF_BIAS;
        }
    struct timeval _ib0, _ib1; struct rusage _ic0, _ic1;
    int ibprof = prof_on();
    if (ibprof) { gettimeofday(&_ib1, NULL); getrusage(RUSAGE_SELF, &_ic1);
        g_prof_ib_pack_ms += prof_ms(&_p0, &_ib1); g_prof_ib_pack_cpu_ms += cpu_ms(&_r0, &_ic1);
        _ib0 = _ib1; _ic0 = _ic1; }
    static GLuint tex = 0;
    if (!tex) {
        glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    } else {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    }
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB,0,GL_LUMINANCE_FLOAT32_ATI,w,4,0,GL_LUMINANCE,GL_FLOAT,lum);
    if (ibprof) { gettimeofday(&_ib1, NULL); getrusage(RUSAGE_SELF, &_ic1);
        g_prof_ib_upload_ms += prof_ms(&_ib0, &_ib1); g_prof_ib_upload_cpu_ms += cpu_ms(&_ic0, &_ic1);
        _ib0 = _ib1; _ic0 = _ic1; }
    static GLhandleARB prog = 0;
    static GLint loc_coeffTex = -1;
    if (!prog) { prog = linkp(vs_plain, fs_idct_batch); loc_coeffTex = glGetUniformLocationARB(prog, "coeffTex"); }
    glUseProgramObjectARB(prog);
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB,tex);
    glUniform1iARB(loc_coeffTex,0);
    glClearColor(0,0,0,0); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_QUADS); glVertex2f(0,0);glVertex2f(w,0);glVertex2f(w,4);glVertex2f(0,4); glEnd();
    /* Item 9 follow-up (2026-08-28): no glFinish() here - see gpu_lumadc_
     * batch's matching comment above for the measured rationale. Note for
     * anyone reading DEBUG_GPU_PROFILE output after this change: the
     * "draw" phase timer below no longer captures real wait time (checkgl()
     * is a cheap client-side poll, not a sync point) - that cost has simply
     * moved into the "read" phase timer, which now covers the actual
     * glReadPixels-driven wait. Total (draw+read) is the number that still
     * means what it used to; the draw/read split itself is no longer
     * meaningful post-this-change. */
    checkgl("idct batch draw");
    if (ibprof) { gettimeofday(&_ib1, NULL); getrusage(RUSAGE_SELF, &_ic1);
        g_prof_ib_draw_ms += prof_ms(&_ib0, &_ib1); g_prof_ib_draw_cpu_ms += cpu_ms(&_ic0, &_ic1);
        _ib0 = _ib1; _ic0 = _ic1; }
    static unsigned char px[IDCT_BATCH_MAX * 4 * 4 * 4];
    glReadPixels(0,0,w,4,GL_RGBA,GL_UNSIGNED_BYTE,px);
    if (ibprof) { gettimeofday(&_ib1, NULL); getrusage(RUSAGE_SELF, &_ic1);
        g_prof_ib_read_ms += prof_ms(&_ib0, &_ib1); g_prof_ib_read_cpu_ms += cpu_ms(&_ic0, &_ic1); }
    for (int b = 0; b < chunk_n; b++)
        for (int row = 0; row < 4; row++)
            for (int col = 0; col < 4; col++) {
                int texel = row * w + (b * 4 + col);
                int hi = px[texel*4], lo = px[texel*4+1];
                out[base + b][row*4+col] = hi*256 + lo - 32768;
            }
    if (prof_on()) { gettimeofday(&_p1, NULL); getrusage(RUSAGE_SELF, &_r1);
        g_prof_idct_ms += prof_ms(&_p0, &_p1); g_prof_idct_cpu_ms += cpu_ms(&_r0, &_r1); g_prof_idct_n++; }
    } /* end chunk loop */
}

/* ================= CPU intra16x16 luma prediction (verified) ================= */

static int clip255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static void pred16x16(int mode, const unsigned char left[16], const unsigned char top[16],
                       unsigned char topleft, int out[16][16]) {
    if (mode == 2) {
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) out[r][c] = top[c];
    } else if (mode == 1) {
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) out[r][c] = left[r];
    } else if (mode == 0) {
        int dc = 0;
        for (int i = 0; i < 16; i++) dc += left[i];
        for (int i = 0; i < 16; i++) dc += top[i];
        dc = (dc + 16) >> 5;
        for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) out[r][c] = dc;
    } else {
        int Hfull = ((int)top[7+1] - (int)top[7-1]);
        for (int k = 2; k <= 8; k++) {
            int a = (7 + k <= 15) ? top[7 + k] : topleft;
            int b = (7 - k >= 0) ? top[7 - k] : topleft;
            Hfull += k * (a - b);
        }
        int Vfull = ((int)left[8] - (int)left[6]);
        for (int k = 2; k <= 8; k++) {
            int a = (7 + k <= 15) ? left[7 + k] : (int)topleft;
            int b = (7 - k >= 0) ? left[7 - k] : (int)topleft;
            Vfull += k * (a - b);
        }
        int Hn = (5 * Hfull + 32) >> 6;
        int Vn = (5 * Vfull + 32) >> 6;
        int a = 16 * ((int)left[15] + (int)top[15] + 1) - 7 * (Vn + Hn);
        for (int r = 0; r < 16; r++) {
            int b = a;
            for (int c = 0; c < 16; c++) {
                out[r][c] = clip255(b >> 5);
                b += Hn;
            }
            a += Vn;
        }
    }
}

/* ================= CPU intra8x8 chroma prediction (new) =================
 * Ported from h264pred_template.c's pred8x8_{dc,horizontal,vertical,plane}
 * "always available" DC variant (both left+top neighbors present) -
 * matches the "interior macroblock only" restriction this whole test
 * already relies on (mb_x>0 && mb_y>0, same as gpu-full-intra16-test). */

static void pred8x8(int mode, const unsigned char left[8], const unsigned char top[8],
                     unsigned char topleft, int out[8][8]) {
    if (mode == 2) { /* Vertical */
        for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) out[r][c] = top[c];
    } else if (mode == 1) { /* Horizontal */
        for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) out[r][c] = left[r];
    } else if (mode == 0) { /* DC - real pred8x8_dc, 4 distinct quadrant DC values */
        int dc0 = 0, dc1 = 0, dc2 = 0;
        for (int i = 0; i < 4; i++) {
            dc0 += left[i] + top[i];
            dc1 += top[4 + i];
            dc2 += left[4 + i];
        }
        int dc0v = (dc0 + 4) >> 3;
        int dc1v = (dc1 + 2) >> 2;
        int dc2v = (dc2 + 2) >> 2;
        int dc3v = (dc1 + dc2 + 4) >> 3;
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) out[r][c] = dc0v;
        for (int r = 0; r < 4; r++) for (int c = 4; c < 8; c++) out[r][c] = dc1v;
        for (int r = 4; r < 8; r++) for (int c = 0; c < 4; c++) out[r][c] = dc2v;
        for (int r = 4; r < 8; r++) for (int c = 4; c < 8; c++) out[r][c] = dc3v;
    } else { /* mode==3, Plane */
        int Hfull = ((int)top[3+1] - (int)top[3-1]);
        for (int k = 2; k <= 4; k++) {
            int a = (3 + k <= 7) ? top[3 + k] : topleft;
            int b = (3 - k >= 0) ? top[3 - k] : topleft;
            Hfull += k * (a - b);
        }
        int Vfull = ((int)left[4] - (int)left[2]);
        for (int k = 2; k <= 4; k++) {
            int a = (3 + k <= 7) ? left[3 + k] : (int)topleft;
            int b = (3 - k >= 0) ? left[3 - k] : (int)topleft;
            Vfull += k * (a - b);
        }
        int Hn = (17 * Hfull + 16) >> 5;
        int Vn = (17 * Vfull + 16) >> 5;
        int a = 16 * ((int)left[7] + (int)top[7] + 1) - 3 * (Vn + Hn);
        for (int r = 0; r < 8; r++) {
            int b = a;
            for (int c = 0; c < 8; c++) {
                out[r][c] = clip255(b >> 5);
                b += Hn;
            }
            a += Vn;
        }
    }
}

/* CPU chroma DC 2x2 Hadamard transform (ff_h264_chroma_dc_dequant_idct,
 * h264idct_template.c) - simple enough (4 in, 4 out) that GPU round-trip
 * overhead isn't worth it; matches the plan's "CPU does serial/simple
 * stages" allowance. dc4[] in={top-left,top-right,bottom-left,
 * bottom-right} block DC values (raw, pre-transform); returns the same
 * 4 positions post-transform. */
static void chroma_dc_transform(const int dc4[4], int qmul, int out4[4]) {
    int a = dc4[0], b = dc4[1], c = dc4[2], d = dc4[3];
    int e = a - b; a = a + b; b = c - d; c = c + d;
    out4[0] = (int)(((long long)(a + c) * qmul) >> 7);
    out4[1] = (int)(((long long)(e + b) * qmul) >> 7);
    out4[2] = (int)(((long long)(a - c) * qmul) >> 7);
    out4[3] = (int)(((long long)(e - b) * qmul) >> 7);
}

/* ================= inter (P-slice) motion compensation, CPU ================
 * Byte-for-byte port of h264qpel_template.c's H264_LOWPASS/H264_MC macros
 * (SIZE=16, PUT-only - this project's scope is list-0/single-reference,
 * so there's never an "avg" combine with a second list) and
 * h264chroma_template.c's H264_CHROMA_MC (SIZE=8, PUT-only). Not yet
 * GPU-accelerated - see the plan for why (this milestone is about
 * getting real inter-macroblock integration correct at all; the luma
 * qpel primitives were already GPU-verified in isolation back in M7,
 * porting those shaders into this live path is natural follow-up work,
 * not attempted this session). CPU-side is also exact here (real int32
 * arithmetic, none of quirk #14's FP24 precision concern that motivated
 * M7's two-pass GPU diagonal-case fix). */

static int clip255_i(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static int qpel_tap6(int a, int b, int c, int d, int e, int f) {
    return (c + d) * 20 - (b + e) * 5 + (a + f);
}

/* dst/src cover a WxH block (W,H <= 16 - the largest real MC partition);
 * src must have 2 valid columns to the left and 3 to the right of the
 * W-wide span (columns -2..W+2). Generalized from this project's
 * original fixed-16x16 version for sub-8x8 partition support (item 3) -
 * the tap6 formula itself is genuinely size-independent (real FFmpeg's
 * own h264qpel_template.c macro-templates this exact body on SIZE for
 * 4/8/16 for the same reason), so this is a mechanical generalization,
 * not a new derivation. dst is always a 16x16-capacity buffer; only the
 * first H rows / W cols are written/valid. */
static void h_lowpass_wh(unsigned char dst[16][16], const unsigned char *src, int stride, int w, int h) {
    for (int r = 0; r < h; r++) {
        const unsigned char *s = src + r * stride;
        for (int c = 0; c < w; c++)
            dst[r][c] = clip255_i((qpel_tap6(s[c-2], s[c-1], s[c], s[c+1], s[c+2], s[c+3]) + 16) >> 5);
    }
}
/* src must have 2 valid rows above and 3 below the H-tall span. */
static void v_lowpass_wh(unsigned char dst[16][16], const unsigned char *src, int stride, int w, int h) {
    for (int c = 0; c < w; c++) {
        const unsigned char *s = src + c;
        for (int r = 0; r < h; r++)
            dst[r][c] = clip255_i((qpel_tap6(s[(r-2)*stride], s[(r-1)*stride], s[r*stride],
                                              s[(r+1)*stride], s[(r+2)*stride], s[(r+3)*stride]) + 16) >> 5);
    }
}
/* True 2-stage diagonal (mc22): horizontal tap6 kept as raw int (no
 * rounding/clipping - real int32 arithmetic, safe on CPU) over an
 * extended row range, then vertical tap6 over that, rounded+clipped
 * only once at the very end - matches op2_put's (x+512)>>10. */
static void hv_lowpass_wh(unsigned char dst[16][16], const unsigned char *src, int stride, int w, int h) {
    int tmp[21][16]; /* rows -2..h+2 relative to the block; 21 rows covers
                       * the largest real partition (h=16, i.e. -2..18). */
    for (int r = -2; r <= h + 2; r++) {
        const unsigned char *s = src + r * stride;
        for (int c = 0; c < w; c++)
            tmp[r+2][c] = qpel_tap6(s[c-2], s[c-1], s[c], s[c+1], s[c+2], s[c+3]);
    }
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++)
            dst[r][c] = clip255_i((qpel_tap6(tmp[r][c], tmp[r+1][c], tmp[r+2][c],
                                              tmp[r+3][c], tmp[r+4][c], tmp[r+5][c]) + 512) >> 10);
}
static int mc_l2(int a, int b) { return (a + b + 1) >> 1; }

/* WxH luma motion compensation for quarter-pel phase (h_phase,v_phase)
 * in 0..3 x 0..3 (matching luma_xy=(mx&3)+((my&3)<<2) = h_phase +
 * v_phase*4). `src` points at the reference frame's full-pel base
 * position for this block (mv>>2 already applied by the caller) - needs
 * 2 pixels of margin left/above and 3 right/below for the 6-tap filter
 * passes, i.e. valid at columns/rows -2..W+2 / -2..H+2. Generalized from
 * the original fixed-16x16 version (item 3) - see h_lowpass_wh's comment
 * for why this is safe. */
static void mc_luma_wh(unsigned char out[16][16], const unsigned char *src, int stride,
                        int w, int h, int h_phase, int v_phase) {
    if (h_phase == 0 && v_phase == 0) {
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++) out[r][c] = src[r*stride+c];
        return;
    }
    if (h_phase == 2 && v_phase == 0) { h_lowpass_wh(out, src, stride, w, h); return; }
    if (h_phase == 0 && v_phase == 2) { v_lowpass_wh(out, src, stride, w, h); return; }
    if (h_phase == 2 && v_phase == 2) { hv_lowpass_wh(out, src, stride, w, h); return; }

    if (v_phase == 0) { /* h_phase in {1,3} */
        unsigned char halfH[16][16]; h_lowpass_wh(halfH, src, stride, w, h);
        int col_off = (h_phase == 3) ? 1 : 0;
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
            out[r][c] = mc_l2(src[r*stride+c+col_off], halfH[r][c]);
        return;
    }
    if (h_phase == 0) { /* v_phase in {1,3} */
        unsigned char halfV[16][16]; v_lowpass_wh(halfV, src, stride, w, h);
        int row_off = (v_phase == 3) ? 1 : 0;
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
            out[r][c] = mc_l2(src[(r+row_off)*stride+c], halfV[r][c]);
        return;
    }
    if (h_phase == 2) { /* v_phase in {1,3} */
        int row_off = (v_phase == 3) ? 1 : 0;
        unsigned char halfH[16][16]; h_lowpass_wh(halfH, src + row_off*stride, stride, w, h);
        unsigned char halfHV[16][16]; hv_lowpass_wh(halfHV, src, stride, w, h);
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
            out[r][c] = mc_l2(halfH[r][c], halfHV[r][c]);
        return;
    }
    if (v_phase == 2) { /* h_phase in {1,3} */
        int col_off = (h_phase == 3) ? 1 : 0;
        unsigned char halfV[16][16]; v_lowpass_wh(halfV, src + col_off, stride, w, h);
        unsigned char halfHV[16][16]; hv_lowpass_wh(halfHV, src, stride, w, h);
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
            out[r][c] = mc_l2(halfV[r][c], halfHV[r][c]);
        return;
    }
    /* both h_phase,v_phase in {1,3} */
    {
        int row_off = (v_phase == 3) ? 1 : 0;
        int col_off = (h_phase == 3) ? 1 : 0;
        unsigned char halfH[16][16]; h_lowpass_wh(halfH, src + row_off*stride, stride, w, h);
        unsigned char halfV[16][16]; v_lowpass_wh(halfV, src + col_off, stride, w, h);
        for (int r = 0; r < h; r++) for (int c = 0; c < w; c++)
            out[r][c] = mc_l2(halfH[r][c], halfV[r][c]);
    }
}
static void mc_luma16(unsigned char out[16][16], const unsigned char *src, int stride,
                       int h_phase, int v_phase) {
    mc_luma_wh(out, src, stride, 16, 16, h_phase, v_phase);
}

/* Chroma bilinear MC, WxH, 1/8-pel phase (x,y each 0..7). `src` points
 * at the reference frame's full-pel chroma base position; needs one
 * extra valid row/column below+right (the 2x2 bilinear tap). Generalized
 * from the original fixed-8x8 version (item 3) - same size-independence
 * rationale as the luma helpers above (real FFmpeg's own
 * h264chroma_template.c macro-templates this on SIZE too). */
static void mc_chroma_wh(unsigned char out[8][8], const unsigned char *src, int stride, int w, int h, int x, int y) {
    int A = (8-x)*(8-y), B = x*(8-y), C = (8-x)*y, D = x*y;
    for (int r = 0; r < h; r++) {
        const unsigned char *s = src + r*stride;
        for (int c = 0; c < w; c++) {
            int v;
            if (D) v = A*s[c] + B*s[c+1] + C*s[stride+c] + D*s[stride+c+1];
            else if (B + C) { int E = B+C; int step = C ? stride : 1; v = A*s[c] + E*s[step+c]; }
            else v = A*s[c];
            out[r][c] = (unsigned char)((v + 32) >> 6);
        }
    }
}
static void mc_chroma8(unsigned char out[8][8], const unsigned char *src, int stride, int x, int y) {
    mc_chroma_wh(out, src, stride, 8, 8, x, y);
}

/* ================= block index -> spatial position tables ================= */

/* Luma: FFmpeg's Z-order quadrant numbering (see gpu-full-intra16-test /
 * plan's "Real integration" section for the scan8[]-derived proof). */
static const int luma_blk_row[16] = {0,0,1,1,0,0,1,1,2,2,3,3,2,2,3,3};
static const int luma_blk_col[16] = {0,1,0,1,2,3,2,3,0,1,0,1,2,3,2,3};
/* This project's own DC-Hadamard shader's raw output index -> real
 * Z-order block index permutation (same derivation as above). */
static const int luma_dc_perm[16] = {0,4,1,5,8,12,9,13,2,6,3,7,10,14,11,15};

/* Chroma: block_offset[16+i] for i=0..3 uses scan8[0..3] only, which for
 * a 2x2 grid collapses to simple row-major (verified against
 * h264_slice.c's block_offset[] init loop): block 0=(0,0) 1=(0,1)
 * 2=(1,0) 3=(1,1), relative to a plane's own 4 AC blocks (Cb=coeffs
 * blocks 16-19, Cr=32-35). No permutation needed for the DC transform
 * either - chroma_dc_transform()'s dc4[] is already fed in this order. */
static const int chroma_blk_row[4] = {0,0,1,1};
static const int chroma_blk_col[4] = {0,1,0,1};

/* ================= full macroblock reconstruction ================= */

/* Reconstructs one real intra16x16 macroblock's luma+chroma pixels
 * entirely (CPU pred + GPU IDCT/DC-transform, all already verified
 * separately) and writes them into the live frame buffer. Returns 1 on
 * success (always succeeds for a well-formed intra16x16 MB - no failure
 * path needed, everything here is unconditional arithmetic). */
/* ================= cross-macroblock batching: pending queue =================
 *
 * Per-macroblock batching (24 blocks -> 1 draw call) was the first win.
 * This extends it across macroblocks: instead of flushing to the GPU
 * once per macroblock, queue up to a whole ROW's worth of qualifying
 * macroblocks' coefficient data and IDCT the entire row in one mega
 * draw call.
 *
 * The correctness constraint that sets the batching granularity: real
 * FFmpeg's per-row decode loop runs loop_filter() for row N as soon as
 * row N finishes (backup_mb_border() inside it saves row N's true
 * pre-filter bottom row into sl->top_borders[] for row N+1 to read) -
 * BEFORE row N+1's macroblocks reconstruct. If we deferred one of row
 * N's macroblocks' pixel writes past that point, loop_filter() would
 * read wrong (prediction-only, no residual yet) pixels for it, and that
 * wrong data would propagate into top_borders[] for row N+1 too. So:
 * every macroblock queued in a row MUST be flushed (pixels finalized)
 * before FFmpeg's own loop advances to the next row. Within a row,
 * queued macroblocks are also flushed early if a not-yet-flushed
 * neighbor would otherwise leave a later same-row macroblock reading
 * its own not-yet-written left-context.
 *
 * The very last row of the frame has no "next row" hook call to detect
 * via the row-change check, so it's caught explicitly via mb_height
 * instead (see the flush call in live_hook). */

typedef struct PendingMB {
    int mb_x, mb_y;
    uint8_t *dest_y, *dest_cb, *dest_cr;
    int linesize, uvlinesize;
    int pred[16][16];
    int cpred[2][8][8];
} PendingMB;

/* Item 9 frame-scale restructure (2026-08-28): grown from 40 (one row) to
 * a whole-frame bound, now that deferring reconstruction across many rows
 * is safe (deblocking is postponed for the whole frame while the hook is
 * installed - see x1900_hook.h's ff_x1900_hook_installed comment). 1024
 * covers this content's real 30x30=900 macroblocks/frame with headroom;
 * the existing g_pending_n>=PENDING_MAX safety cap still applies if a
 * future frame ever needs more (degrades to an earlier flush, not an
 * overflow). GPU dispatch functions that batch from this queue
 * (gpu_idct_batch, dispatch_singlepass_group, dispatch_diag_group) either
 * already chunk internally past GL_MAX_TEXTURE_SIZE (MC's dispatchers,
 * proven in item 4) or gained chunking as part of this same restructure
 * (gpu_idct_batch, see its own comment) - this constant no longer needs
 * to itself stay under the single-dispatch texture-width limit. */
#define PENDING_MAX 1024
static PendingMB g_pending[PENDING_MAX];
static int g_pending_n = 0;
static int g_pending_coeffs[PENDING_MAX * 24][16];

static const int luma_ac_scan8[16] = {12,13,20,21,14,15,22,23,28,29,36,37,30,31,38,39};
static const int chroma_ac_scan8[2][4] = {
    {52, 53, 60, 61},   /* Cb: scan8[16],scan8[17],scan8[18],scan8[19] */
    {92, 93, 100, 101}, /* Cr: scan8[32],scan8[33],scan8[34],scan8[35] */
};

/* ================= item 4, phase 4d: GPU-accelerated luma MC ================
 *
 * Defers mc_luma_wh's per-partition CPU call through the same PendingMB
 * queue the IDCT batch already uses, so luma MC batches across whole rows
 * of macroblocks exactly like item 3's residual reconstruction - see the
 * plan's "Item 4 (GPU-accelerating MC): DESIGN PASS" and "Item 4, phases
 * 4a-4c IMPLEMENTED" sections for the two proven building blocks this
 * reuses verbatim: the "7 unconditional values, branch only to select"
 * single-pass-family shader (fs_mc_batch, gpu-mc-singlepass-test) and the
 * quirk-#14 two-pass diagonal-family shader (fs_diag_stage1/stage2, same
 * file). Chroma MC stays CPU (mc_chroma_wh, unchanged) - real, separate
 * follow-up work per the design pass, not attempted here.
 *
 * One real generalization beyond both proven tests: partitions here have
 * genuinely mixed real widths (16/8/4), not the fixed 16-wide blocks those
 * tests used - the single-pass-family batch resolves this via a per-COLUMN
 * lookup texture (colInfoTex, the design pass's own name for this),
 * mirroring blockInfoTex's per-block lookup exactly. The diagonal-family
 * batch instead keeps 4c's fixed-16-wide-per-block layout (this project's
 * established "always pay worst case, never branch on size" convention,
 * already used for HEIGHT here) since diagonal MVs are a small fraction of
 * real content - not worth extending colInfoTex to the two-pass pipeline
 * for now.
 *
 * Real, deliberate deviation flagged by the design pass: the diagonal
 * family's GPU path targets a ROUNDED-intermediate formula (forced by
 * quirk #14's FP24 limit), not bit-identical to this project's CPU-side
 * hv_lowpass_wh (FFmpeg's real unrounded-intermediate algorithm, safe
 * there on real int32 arithmetic) - diagonal-phase blocks may very
 * slightly deviate from the byte-exact CPU baseline once this lands.
 * Expected and inherent to the hardware, not a regression to chase. */

typedef struct McReq {
    int pend_idx;                 /* which g_pending[] slot to write into */
    int x_off, y_off, w, h;       /* destination rectangle within that
                                    * slot's 16x16 luma pred[][] */
    int pel_x, pel_y;             /* integer reference-frame source
                                    * position (already margin-checked by
                                    * the caller) */
    int h_phase, v_phase;
    const unsigned char *ref_y;
    int ref_linesize;
} McReq;

/* Same worst case as PENDING_MAX's own derivation, generalized: a single
 * macroblock can contribute up to 16 partitions (P_8x8 with every quadrant
 * at 4x4 granularity), so a full 40-wide row can queue up to 40*16 luma MC
 * requests between flushes. */
#define MC_PENDING_MAX (PENDING_MAX * 16)
static McReq g_mc_pending[MC_PENDING_MAX];
static int g_mc_pending_n = 0;

/* Item 10 follow-up: defers gpu_lumadc_batch's input the same way McReq
 * defers MC's - one request per intra macroblock (never more, unlike MC's
 * up-to-16-partitions/MB), resolved by resolve_lumadc_pending() right
 * before flush_pending() calls gpu_idct_batch (the luma coefficient array
 * it reads needs each intra block's real DC value patched in first). */
typedef struct LumaDCReq {
    int pend_idx;
    int dc16[16];
    int qmul;
} LumaDCReq;
static LumaDCReq g_lumadc_pending[PENDING_MAX];
static int g_lumadc_pending_n = 0;

/* This macroblock's own partitions, accumulated by compute_mc_pred/
 * compute_mc_part as they run, before enqueue_reconstruction knows which
 * g_pending[] slot they belong to. Reset once per live_hook call (not
 * once per successful enqueue) - see live_hook's own comment on why a
 * macroblock that pushes some partitions then declines must not leak
 * them into whichever macroblock enqueues next. */
static McReq g_cur_mb_mc[16];
static int g_cur_mb_mc_n = 0;

static int g_mc_frame_w = 0, g_mc_frame_h = 0;

static void push_mc_req(int x_off, int y_off, int w, int h, int pel_x, int pel_y,
                         int h_phase, int v_phase, const unsigned char *ref_y, int ref_linesize) {
    if (g_cur_mb_mc_n >= 16) return; /* can't happen - at most 16 partitions/MB */
    McReq *r = &g_cur_mb_mc[g_cur_mb_mc_n++];
    r->x_off = x_off; r->y_off = y_off; r->w = w; r->h = h;
    r->pel_x = pel_x; r->pel_y = pel_y; r->h_phase = h_phase; r->v_phase = v_phase;
    r->ref_y = ref_y; r->ref_linesize = ref_linesize;
}

static int is_diag_phase(int h, int v) {
    if (h == 2 && v == 2) return 1;
    if (v == 2 && (h == 1 || h == 3)) return 1;
    if (h == 2 && (v == 1 || v == 3)) return 1;
    return 0;
}

/* Reference-frame texture cache, keyed by the raw plane pointer - real
 * payoff is NOT re-uploading a whole reference frame (a real, measurable
 * cost) on every one of a frame's many row-flushes when they all share the
 * same single reference, which is the common case for this content.
 * Reset once per decode_to_frame() call (see there) since FFmpeg's frame
 * pool can in principle recycle a buffer address across different decode
 * runs - never observed to matter within one run's own small reference
 * set, but cheap to close off entirely rather than assume.
 *
 * Item 10 fixes, round 2 (2026-08-28): grown from 4 to 16, empirically
 * tuned on a real 40-frame continuous run (DEBUG_GPU_PROFILE). Real measured
 * cause of resolve_mc_pending's per-call cost growing over a longer run
 * (~7ms/call early -> ~27ms/call by frame ~30, see the plan's "Item 10
 * fixes, round 1" write-up): deeper-GOP frames genuinely reference 5+
 * distinct pictures (already documented project-wide), and eviction here is
 * plain round-robin, not LRU - once a frame touches more than MC_REFTEX_MAX
 * distinct references, any of them can get evicted and then immediately
 * miss again on its very next use, forcing a full reference-frame texture
 * re-upload (`w*h` floats, ~3.7MB for this content's 480x480 luma plane)
 * instead of a cache hit. Measured 4->8->16->32 directly: 8 roughly halved
 * resolve_mc_pending's total cost (13786ms->6689ms over the 40-frame run),
 * 16 nearly halved it again (6689ms->4294ms), 32 plateaued (4237ms, within
 * noise of 16) - 16 already covers this content's real working set, 32
 * buys nothing. Cheap either way (this struct is tiny per slot), but no
 * reason to keep the unused headroom. */
#define MC_REFTEX_MAX 16
static struct {
    const unsigned char *ptr;
    int w, h, stride;
    GLuint tex;
} g_reftex_cache[MC_REFTEX_MAX];
static int g_reftex_n = 0, g_reftex_next = 0;

static void reftex_cache_reset(void) {
    for (int i = 0; i < MC_REFTEX_MAX; i++) g_reftex_cache[i].ptr = NULL;
    g_reftex_n = 0; g_reftex_next = 0;
}

static double g_prof_reftex_hit_ms = 0, g_prof_reftex_hit_cpu_ms = 0;
static double g_prof_reftex_miss_ms = 0, g_prof_reftex_miss_cpu_ms = 0;
static int g_prof_reftex_hit_n = 0, g_prof_reftex_miss_n = 0;
static GLuint reftex_lookup_or_upload(const unsigned char *ref_y, int stride, int w, int h) {
    struct timeval _rt0, _rt1; struct rusage _ru0, _ru1;
    int rprof = prof_on();
    if (rprof) { gettimeofday(&_rt0, NULL); getrusage(RUSAGE_SELF, &_ru0); }
    for (int i = 0; i < g_reftex_n; i++)
        if (g_reftex_cache[i].ptr == ref_y && g_reftex_cache[i].w == w && g_reftex_cache[i].h == h) {
            if (rprof) { gettimeofday(&_rt1, NULL); getrusage(RUSAGE_SELF, &_ru1);
                g_prof_reftex_hit_ms += prof_ms(&_rt0, &_rt1); g_prof_reftex_hit_cpu_ms += cpu_ms(&_ru0, &_ru1);
                g_prof_reftex_hit_n++; }
            return g_reftex_cache[i].tex;
        }

    int slot;
    if (g_reftex_n < MC_REFTEX_MAX) { slot = g_reftex_n++; }
    else { slot = g_reftex_next; g_reftex_next = (g_reftex_next + 1) % MC_REFTEX_MAX; }

    GLuint tex = g_reftex_cache[slot].tex;
    if (!tex) {
        glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    } else {
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    }
    /* Item 9 fix (2026-08-28): this was GL_RGBA_FLOAT32_ATI with a CPU-side
     * per-pixel packing loop (srow[x]/255.0f into a 4-float RGBA buffer) -
     * measured at 100% CPU, 17.49ms/miss, ~38% of this whole run's total
     * live-decode time (116 misses, 2029ms). Real cause: every shader that
     * samples refTex already does `texture2DRect(refTex,...).r*255.0`
     * (confirmed by grepping every real call site) - i.e. it already
     * expects a NORMALIZED [0,1] read, which a plain 8-bit texture format
     * provides automatically via OpenGL's standard fixed-point
     * normalization, with no float conversion needed at all. Switched to
     * GL_LUMINANCE8 (a core OpenGL 1.1 format, not an ATI extension - lower
     * risk than the float format this replaces, not higher) and
     * GL_UNPACK_ROW_LENGTH to let the driver read directly from ref_y's
     * own stride - eliminates the CPU packing loop AND its staging buffer
     * entirely, not just shrinks it. Zero shader changes needed (verified:
     * every texture2DRect(refTex,...) call already multiplies by 255.0). */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride);
    glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_LUMINANCE8, w, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, ref_y);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    checkgl("reftex upload (GL_LUMINANCE8)");
    g_reftex_cache[slot].ptr = ref_y; g_reftex_cache[slot].w = w; g_reftex_cache[slot].h = h;
    g_reftex_cache[slot].stride = stride; g_reftex_cache[slot].tex = tex;
    if (rprof) { gettimeofday(&_rt1, NULL); getrusage(RUSAGE_SELF, &_ru1);
        g_prof_reftex_miss_ms += prof_ms(&_rt0, &_rt1); g_prof_reftex_miss_cpu_ms += cpu_ms(&_ru0, &_ru1);
        g_prof_reftex_miss_n++; }
    return tex;
}

/* ---- single-pass-family batch: fs_mc_batch (gpu-mc-singlepass-test,
 * phase 4b) generalized from a fixed blockWidth uniform to a per-column
 * (blockIndex, localX) lookup texture, so blocks of genuinely different
 * real widths can pack into one draw call. Every line of the actual MC
 * formula below is unchanged from the already-verified fs_mc_batch. ---- */
static const char *fs_mc_batch_var =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"uniform sampler2DRect colInfoTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main(){\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float row = floor(gl_FragCoord.y);\n"
"  vec4 colInfo = texture2DRect(colInfoTex, vec2(col+0.5, 0.5));\n"
"  float blockIdx = colInfo.r;\n"
"  float localX = colInfo.g;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx+0.5, 0.5));\n"
"  vec2 b = vec2(info.r + localX, info.g + row);\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  float full00 = texture2DRect(refTex, b).r*255.0;\n"
"  float full10 = texture2DRect(refTex, b+vec2(1.0,0.0)).r*255.0;\n"
"  float full01 = texture2DRect(refTex, b+vec2(0.0,1.0)).r*255.0;\n"
"  float halfH0 = halfH(b);\n"
"  float halfH1 = halfH(b+vec2(0.0,1.0));\n"
"  float halfV0 = halfV(b);\n"
"  float halfV1 = halfV(b+vec2(1.0,0.0));\n"
"  float result = full00;\n"
"  if (hPhase == 0.0 && vPhase == 0.0) {\n"
"    result = full00;\n"
"  } else if (hPhase == 2.0 && vPhase == 0.0) {\n"
"    result = halfH0;\n"
"  } else if (hPhase == 0.0 && vPhase == 2.0) {\n"
"    result = halfV0;\n"
"  } else if (vPhase == 0.0) {\n"
"    float fullOp = full00; if (hPhase == 3.0) fullOp = full10;\n"
"    result = floor((fullOp + halfH0 + 1.0) / 2.0);\n"
"  } else if (hPhase == 0.0) {\n"
"    float fullOp = full00; if (vPhase == 3.0) fullOp = full01;\n"
"    result = floor((fullOp + halfV0 + 1.0) / 2.0);\n"
"  } else {\n"
"    float hOp = halfH0; if (vPhase == 3.0) hOp = halfH1;\n"
"    float vOp = halfV0; if (hPhase == 3.0) vOp = halfV1;\n"
"    result = floor((hOp + vOp + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

/* fs_diag_stage1_batch/fs_diag_stage2_batch (M7's original quirk-#14
 * two-pass fix) removed 2026-08-28 - superseded by fs_diag_singlepass_batch
 * below (git history has the removed source if ever needed again). This
 * single-pass replacement removes the old FBO round trip entirely. Built after
 * precision-boundary-probe proved this ALU is true FP32 (not FP24 -
 * quirk #14's original premise), then diag-singlepass-verify directly
 * compared this exact formula against 5 real captured diagonal-phase
 * cases: it beat the two-pass shader against the TRUE (real FFmpeg,
 * unrounded-intermediate) reference by ~9x fewer mismatched pixels
 * (14/1280 vs. 132/1280, both worst-case off-by-1 - ordinary floor/round
 * boundary noise, not precision loss). The two-pass split was solving a
 * problem that didn't exist on real hardware - see plan.md's "Precision
 * question (§4): SETTLED" and "workaround removed" entries. Keeps the
 * horizontal 6-tap sum as a raw, unrounded value in a shader register
 * (tap6raw) instead of routing it through an intermediate texture -
 * matches this project's own CPU-side hv_lowpass_wh algorithm exactly,
 * one draw call instead of two, no FBO involved at all (so quirk #16's
 * FBO-resize-corruption concern no longer applies to this path - the
 * chunk cap was raised from 256 to MC_BATCH_MAXW/4096 the same day this
 * landed, once that stopped being a constraint - see
 * dispatch_diag_group's own comment). */
static const char *fs_diag_singlepass_batch =
"uniform sampler2DRect refTex;\n"
"uniform sampler2DRect blockInfoTex;\n"
"float clip255(float v) { return max(0.0, min(255.0, v)); }\n"
"float tap6raw(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  return (a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"}\n"
"float diagExact(vec2 b) {\n"
"  float hm2=tap6raw(b+vec2(0.0,-2.0));\n"
"  float hm1=tap6raw(b+vec2(0.0,-1.0));\n"
"  float h0 =tap6raw(b+vec2(0.0, 0.0));\n"
"  float h1 =tap6raw(b+vec2(0.0, 1.0));\n"
"  float h2 =tap6raw(b+vec2(0.0, 2.0));\n"
"  float h3 =tap6raw(b+vec2(0.0, 3.0));\n"
"  float v = (h0+h1)*20.0 - (hm1+h2)*5.0 + (hm2+h3);\n"
"  return clip255(floor((v+512.0)/1024.0));\n"
"}\n"
"float halfH(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(-2.0,0.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(-1.0,0.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2( 0.0,0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2( 1.0,0.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2( 2.0,0.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2( 3.0,0.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"float halfV(vec2 b) {\n"
"  float a2=texture2DRect(refTex,b+vec2(0.0,-2.0)).r*255.0;\n"
"  float a1=texture2DRect(refTex,b+vec2(0.0,-1.0)).r*255.0;\n"
"  float a0=texture2DRect(refTex,b+vec2(0.0, 0.0)).r*255.0;\n"
"  float a3=texture2DRect(refTex,b+vec2(0.0, 1.0)).r*255.0;\n"
"  float a4=texture2DRect(refTex,b+vec2(0.0, 2.0)).r*255.0;\n"
"  float a5=texture2DRect(refTex,b+vec2(0.0, 3.0)).r*255.0;\n"
"  float v=(a0+a3)*20.0-(a1+a4)*5.0+(a2+a5);\n"
"  return clip255(floor((v+16.0)/32.0));\n"
"}\n"
"void main() {\n"
"  float col = floor(gl_FragCoord.x);\n"
"  float rowr = floor(gl_FragCoord.y);\n"
"  float blockIdx = floor(col / 16.0);\n"
"  float localX = col - blockIdx * 16.0;\n"
"  vec4 info = texture2DRect(blockInfoTex, vec2(blockIdx + 0.5, 0.5));\n"
"  float hPhase = info.b, vPhase = info.a;\n"
"  vec2 b = vec2(info.r + localX + 0.5, info.g + rowr + 0.5);\n"
"  float diag = diagExact(b);\n"
"  float result = diag;\n"
"  if (hPhase == 2.0 && vPhase == 2.0) {\n"
"    result = diag;\n"
"  } else if (hPhase == 2.0) {\n"
"    float ro = (vPhase == 3.0) ? 1.0 : 0.0;\n"
"    float hOp = halfH(b + vec2(0.0, ro));\n"
"    result = floor((hOp + diag + 1.0) / 2.0);\n"
"  } else {\n"
"    float co = (hPhase == 3.0) ? 1.0 : 0.0;\n"
"    float vOp = halfV(b + vec2(co, 0.0));\n"
"    result = floor((vOp + diag + 1.0) / 2.0);\n"
"  }\n"
"  gl_FragColor = vec4(result/255.0, 0.0, 0.0, 1.0);\n"
"}\n";

/* Batches (up to) MC_BATCH_MAXW output columns per draw call - generous
 * headroom under this driver's GL_MAX_TEXTURE_SIZE=4096 (M4 probe) and
 * this test's own 4096-wide Pbuffer, chunked below for the (currently
 * theoretical, never observed) case a single row's requests exceed it. */
#define MC_BATCH_MAXW 4096

static void dispatch_singlepass_group(const unsigned char *ref_y, int ref_stride, McReq **reqs, int n) {
    GLuint refTex = reftex_lookup_or_upload(ref_y, ref_stride, g_mc_frame_w, g_mc_frame_h);
    static GLuint blockInfoTex = 0, colInfoTex = 0;
    static GLhandleARB prog = 0;
    static GLint loc_refTex = -1, loc_blockInfoTex = -1, loc_colInfoTex = -1;
    if (!prog) {
        prog = linkp(vs_plain, fs_mc_batch_var);
        loc_refTex = glGetUniformLocationARB(prog, "refTex");
        loc_blockInfoTex = glGetUniformLocationARB(prog, "blockInfoTex");
        loc_colInfoTex = glGetUniformLocationARB(prog, "colInfoTex");
    }
    if (!blockInfoTex) {
        glGenTextures(1, &blockInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    if (!colInfoTex) {
        glGenTextures(1, &colInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    static float blockinfo[MC_PENDING_MAX * 4];
    static float colinfo[MC_BATCH_MAXW * 4];
    static unsigned char pixels[MC_BATCH_MAXW * 16 * 4];
    static int colStart[MC_PENDING_MAX];

    int start = 0;
    while (start < n) {
        int cnt = 0, vw = 0;
        while (start + cnt < n && vw + reqs[start+cnt]->w <= MC_BATCH_MAXW) { vw += reqs[start+cnt]->w; cnt++; }
        if (cnt == 0) { cnt = 1; vw = reqs[start]->w; } /* one oversized block alone, never observed for real content */

        struct timeval _t0, _t1; struct rusage _u0, _u1;
        int prof = prof_on();
        if (prof) { gettimeofday(&_t0, NULL); getrusage(RUSAGE_SELF, &_u0); }

        int col_cursor = 0;
        for (int i = 0; i < cnt; i++) {
            McReq *r = reqs[start + i];
            blockinfo[i*4+0] = (float)r->pel_x; blockinfo[i*4+1] = (float)r->pel_y;
            blockinfo[i*4+2] = (float)r->h_phase; blockinfo[i*4+3] = (float)r->v_phase;
            colStart[i] = col_cursor;
            for (int c = 0; c < r->w; c++) {
                colinfo[col_cursor*4+0] = (float)i; colinfo[col_cursor*4+1] = (float)c;
                colinfo[col_cursor*4+2] = 0; colinfo[col_cursor*4+3] = 1;
                col_cursor++;
            }
        }
        if (prof) { gettimeofday(&_t1, NULL); getrusage(RUSAGE_SELF, &_u1);
            g_prof_sp_pack_ms += prof_ms(&_t0, &_t1); g_prof_sp_pack_cpu_ms += cpu_ms(&_u0, &_u1);
            _t0 = _t1; _u0 = _u1; }

        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, cnt, 1, 0, GL_RGBA, GL_FLOAT, blockinfo);
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, vw, 1, 0, GL_RGBA, GL_FLOAT, colinfo);
        checkgl("mc singlepass batch upload");
        if (prof) { gettimeofday(&_t1, NULL); getrusage(RUSAGE_SELF, &_u1);
            g_prof_sp_upload_ms += prof_ms(&_t0, &_t1); g_prof_sp_upload_cpu_ms += cpu_ms(&_u0, &_u1);
            _t0 = _t1; _u0 = _u1; }

        glViewport(0, 0, vw, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(prog);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
        glUniform1iARB(loc_refTex, 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glUniform1iARB(loc_blockInfoTex, 1);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, colInfoTex);
        glUniform1iARB(loc_colInfoTex, 2);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(vw,0); glVertex2f(vw,16); glVertex2f(0,16); glEnd();
        /* Item 9 follow-up (2026-08-28): no glFinish() here - see
         * gpu_lumadc_batch's comment for the measured rationale, and
         * gpu_idct_batch's for the profiling-semantics note (applies here
         * too: draw-phase timing below no longer reflects real wait time,
         * that cost moved into the read-phase timer). */
        checkgl("mc singlepass batch draw");
        if (prof) { gettimeofday(&_t1, NULL); getrusage(RUSAGE_SELF, &_u1);
            g_prof_sp_draw_ms += prof_ms(&_t0, &_t1); g_prof_sp_draw_cpu_ms += cpu_ms(&_u0, &_u1);
            _t0 = _t1; _u0 = _u1; }

        glReadPixels(0, 0, vw, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        for (int i = 0; i < cnt; i++) {
            McReq *r = reqs[start + i];
            PendingMB *p = &g_pending[r->pend_idx];
            for (int row = 0; row < r->h; row++)
                for (int c = 0; c < r->w; c++)
                    p->pred[r->y_off + row][r->x_off + c] = pixels[(row*vw + colStart[i] + c) * 4];
        }
        if (prof) { gettimeofday(&_t1, NULL); getrusage(RUSAGE_SELF, &_u1);
            g_prof_sp_read_ms += prof_ms(&_t0, &_t1); g_prof_sp_read_cpu_ms += cpu_ms(&_u0, &_u1); }
        if (prof) g_prof_sp_chunks++;
        start += cnt;
    }
}

/* Real driver quirk found integrating the ORIGINAL two-pass diagonal-MC
 * shader (not in the ATI_RADEON_X1900_TIGER_DRIVER_QUIRKS catalog, and
 * distinct from quirk #15) - kept here as a standing warning for any
 * FUTURE FBO work in this file, even though the specific path that hit it
 * (dispatch_diag_group's old FBO-based stage1) was removed 2026-08-28:
 * FBO-attached render-target rendering has its own width limit around
 * 256px - independent of GL_MAX_TEXTURE_SIZE=4096 (which only bounds a
 * texture's SAMPLED use, already probed and confirmed fine at that size
 * elsewhere in this project) - AND, worse, repeatedly re-sizing an FBO's
 * attached texture via glTexImage2D to a NEW width on every call
 * progressively corrupts rendering at ever-smaller widths (found via a
 * standalone probe, `fbo_width_probe.c`/`fbo_width_probe2.c`: a texture
 * resized 16->32->40->44...->256 across successive calls broke starting
 * around col 32-40, while the exact same widths tested fresh-process-per-
 * width, or against a texture allocated ONCE at a fixed size and only
 * re-VIEWPORTed smaller, were perfect up to 256 every time). Fix for any
 * future FBO here: allocate it ONCE at a fixed width and never
 * glTexImage2D it again - only the glViewport/quad extent should vary per
 * dispatch. Candidate for a new quirk entry (#16?) in the
 * ati-x1900-driver-quirks skill if the user wants to contribute it back -
 * not done automatically. */

static double g_prof_diag_ms = 0, g_prof_diag_cpu_ms = 0;
static int g_prof_diag_n = 0, g_prof_diag_chunks = 0;
static void dispatch_diag_group(const unsigned char *ref_y, int ref_stride, McReq **reqs, int n) {
    struct timeval _d0, _d1; struct rusage _e0, _e1;
    int diagprof = prof_on();
    if (diagprof) { gettimeofday(&_d0, NULL); getrusage(RUSAGE_SELF, &_e0); g_prof_diag_n++; }
    GLuint refTex = reftex_lookup_or_upload(ref_y, ref_stride, g_mc_frame_w, g_mc_frame_h);
    static GLuint blockInfoTex = 0;
    static GLhandleARB prog = 0;
    static GLint loc_refTex = -1, loc_blockInfoTex = -1;
    if (!prog) {
        prog = linkp(vs_plain, fs_diag_singlepass_batch);
        loc_refTex = glGetUniformLocationARB(prog, "refTex");
        loc_blockInfoTex = glGetUniformLocationARB(prog, "blockInfoTex");
    }
    if (!blockInfoTex) {
        glGenTextures(1, &blockInfoTex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }

    static float blockinfo[MC_PENDING_MAX * 4];
    static unsigned char pixels[MC_BATCH_MAXW * 16 * 4];
    /* Raised 2026-08-28 from MC_DIAG_FBO_MAXW (256) to MC_BATCH_MAXW (4096):
     * that old, smaller cap existed only because of the now-removed FBO's
     * repeated-resize corruption bug (quirk #16) - this path renders
     * straight to the default framebuffer now, same as
     * dispatch_singlepass_group, which already proved MC_BATCH_MAXW-wide
     * viewports safe on this driver/Pbuffer. pixels[] above is already
     * sized for the full MC_BATCH_MAXW width, so no buffer-size change
     * needed here, only the chunk-size cap. */
    int maxBlocksPerChunk = getenv("MC_DIAG_CHUNKN") ? atoi(getenv("MC_DIAG_CHUNKN")) : (MC_BATCH_MAXW / 16);

    int start = 0;
    while (start < n) {
        int cnt = (n - start < maxBlocksPerChunk) ? (n - start) : maxBlocksPerChunk;
        int vw = cnt * 16;

        for (int i = 0; i < cnt; i++) {
            McReq *r = reqs[start + i];
            blockinfo[i*4+0] = (float)r->pel_x; blockinfo[i*4+1] = (float)r->pel_y;
            blockinfo[i*4+2] = (float)r->h_phase; blockinfo[i*4+3] = (float)r->v_phase;
        }
        glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glTexImage2D(GL_TEXTURE_RECTANGLE_ARB, 0, GL_RGBA_FLOAT32_ATI, cnt, 1, 0, GL_RGBA, GL_FLOAT, blockinfo);
        checkgl("mc diag blockinfo upload");

        /* Single pass, straight to the Pbuffer's own default framebuffer -
         * no FBO, no intermediate texture, no second draw call. */
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0);
        glViewport(0, 0, vw, 16);
        glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho(0, vw, 0, 16, -1, 1);
        glMatrixMode(GL_MODELVIEW); glLoadIdentity();
        glUseProgramObjectARB(prog);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, refTex);
        glUniform1iARB(loc_refTex, 0);
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, blockInfoTex);
        glUniform1iARB(loc_blockInfoTex, 1);
        glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
        glBegin(GL_QUADS); glVertex2f(0,0); glVertex2f(vw,0); glVertex2f(vw,16); glVertex2f(0,16); glEnd();
        /* No glFinish() here - matches every other hot-path dispatch in
         * this file post-item-9 (the glReadPixels right below is the real
         * sync point). */
        checkgl("mc diag singlepass draw");

        glReadPixels(0, 0, vw, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
        if (getenv("DEBUG_MC_DIAG_VERIFY")) {
            for (int i = 0; i < cnt; i++) {
                McReq *r = reqs[start + i];
                unsigned char cpuluma[16][16];
                mc_luma_wh(cpuluma, r->ref_y + r->pel_y * r->ref_linesize + r->pel_x, r->ref_linesize,
                           r->w, r->h, r->h_phase, r->v_phase);
                int mism = 0, worst = 0, firstrow = -1, firstcol = -1, firstgpu = 0, firstcpu = 0;
                for (int row = 0; row < r->h; row++) for (int c = 0; c < r->w; c++) {
                    int gpu_v = pixels[(row*vw + i*16 + c) * 4];
                    int cpu_v = cpuluma[row][c];
                    int d = abs(gpu_v - cpu_v);
                    if (d > 1) { mism++; if (firstrow < 0) { firstrow=row; firstcol=c; firstgpu=gpu_v; firstcpu=cpu_v; } }
                    if (d > worst) worst = d;
                }
                if (mism > 0)
                    fprintf(stderr, "DIAG block#%d (chunk-local) pel=(%d,%d) hphase=%d vphase=%d w=%d h=%d: "
                            "%d/%d mismatch worst=%d first@(%d,%d) gpu=%d cpu=%d\n",
                            i, r->pel_x, r->pel_y, r->h_phase, r->v_phase, r->w, r->h,
                            mism, r->w*r->h, worst, firstrow, firstcol, firstgpu, firstcpu);
            }
        }
        for (int i = 0; i < cnt; i++) {
            McReq *r = reqs[start + i];
            PendingMB *p = &g_pending[r->pend_idx];
            for (int row = 0; row < r->h; row++)
                for (int c = 0; c < r->w; c++)
                    p->pred[r->y_off + row][r->x_off + c] = pixels[(row*vw + i*16 + c) * 4];
        }
        start += cnt;
        if (diagprof) g_prof_diag_chunks++;
    }
    if (diagprof) { gettimeofday(&_d1, NULL); getrusage(RUSAGE_SELF, &_e1);
        g_prof_diag_ms += prof_ms(&_d0, &_d1); g_prof_diag_cpu_ms += cpu_ms(&_e0, &_e1); }
}

/* Groups this flush's queued luma MC requests by (family, reference
 * pointer) and issues one batched dispatch per group - see this section's
 * own comment for why family needs separate shaders (quirk #14) and
 * reference needs separate texture uploads (no texture-array/indexed-
 * sampling primitive on this driver). Previously "in practice almost
 * every flush has exactly one reference pointer" (true when a flush was
 * only ever one row's worth of content) - item 9's frame-scale restructure
 * (2026-08-28) means a single flush can now legitimately span a whole
 * frame, which item 10's own profiling already showed can genuinely touch
 * 16 distinct references (the finding behind MC_REFTEX_MAX=16) - grown to
 * match, with headroom, since silently capping this would truncate real
 * groups (any macroblock referencing a distinct pointer past this cap
 * would never get grouped/dispatched at all). */
#define MC_GROUP_MAX 32

/* Diagnostic tool (kept, matching this file's established DEBUG_-env-var
 * convention): resolve a list of requests via the original CPU mc_luma_wh
 * instead of any GPU dispatch - isolates whether a future live-path
 * mismatch comes from the queue/wiring itself or from a specific GPU
 * shader dispatch. This is exactly the technique that cracked the real bug
 * behind item 4d's initial integration (a driver quirk in FBO-attached-
 * texture rendering - see dispatch_diag_group's own comment): the
 * MC_CPU_FALLBACK, MC_CPU_SINGLEPASS and MC_CPU_DIAG env vars below
 * bisected which family was at fault before DEBUG_MC_DIAG_STAGE1's stage1
 * dump and MC_DIAG_CHUNKN's chunk-size sweep localized it further. */
static void mc_cpu_fallback_reqs(McReq **reqs, int n) {
    for (int i = 0; i < n; i++) {
        McReq *r = reqs[i];
        PendingMB *p = &g_pending[r->pend_idx];
        unsigned char luma[16][16];
        mc_luma_wh(luma, r->ref_y + r->pel_y * r->ref_linesize + r->pel_x, r->ref_linesize,
                   r->w, r->h, r->h_phase, r->v_phase);
        for (int row = 0; row < r->h; row++)
            for (int c = 0; c < r->w; c++)
                p->pred[r->y_off + row][r->x_off + c] = luma[row][c];
    }
}

static void resolve_mc_pending(void) {
    if (g_mc_pending_n == 0) return;
    struct timeval _p0, _p1; struct rusage _r0, _r1;
    if (prof_on()) { gettimeofday(&_p0, NULL); getrusage(RUSAGE_SELF, &_r0); }
    if (getenv("DEBUG_MC_BATCH"))
        fprintf(stderr, "resolve_mc_pending: n=%d\n", g_mc_pending_n);

    if (getenv("MC_CPU_FALLBACK")) {
        static McReq *all[MC_PENDING_MAX];
        for (int i = 0; i < g_mc_pending_n; i++) all[i] = &g_mc_pending[i];
        mc_cpu_fallback_reqs(all, g_mc_pending_n);
        return;
    }

    static const unsigned char *refptrs[MC_GROUP_MAX];
    static int refstride[MC_GROUP_MAX];
    int nref = 0;
    for (int i = 0; i < g_mc_pending_n; i++) {
        const unsigned char *rp = g_mc_pending[i].ref_y;
        int found = 0;
        for (int j = 0; j < nref; j++) if (refptrs[j] == rp) { found = 1; break; }
        if (!found && nref < MC_GROUP_MAX) { refptrs[nref] = rp; refstride[nref] = g_mc_pending[i].ref_linesize; nref++; }
    }

    static McReq *singlepass[MC_PENDING_MAX];
    static McReq *diagg[MC_PENDING_MAX];
    for (int g = 0; g < nref; g++) {
        int nsp = 0, ndiag = 0;
        for (int i = 0; i < g_mc_pending_n; i++) {
            if (g_mc_pending[i].ref_y != refptrs[g]) continue;
            if (is_diag_phase(g_mc_pending[i].h_phase, g_mc_pending[i].v_phase)) diagg[ndiag++] = &g_mc_pending[i];
            else singlepass[nsp++] = &g_mc_pending[i];
        }
        if (getenv("DEBUG_MC_BATCH"))
            fprintf(stderr, "  group ref=%p: nsp=%d ndiag=%d\n", (const void*)refptrs[g], nsp, ndiag);
        /* Per-family CPU-fallback diagnostics, see mc_cpu_fallback_reqs. */
        if (nsp) {
            if (getenv("MC_CPU_SINGLEPASS")) mc_cpu_fallback_reqs(singlepass, nsp);
            else dispatch_singlepass_group(refptrs[g], refstride[g], singlepass, nsp);
        }
        if (ndiag) {
            if (getenv("MC_CPU_DIAG")) mc_cpu_fallback_reqs(diagg, ndiag);
            else dispatch_diag_group(refptrs[g], refstride[g], diagg, ndiag);
        }
    }
    if (prof_on()) { gettimeofday(&_p1, NULL); getrusage(RUSAGE_SELF, &_r1);
        g_prof_mc_ms += prof_ms(&_p0, &_p1); g_prof_mc_cpu_ms += cpu_ms(&_r0, &_r1); g_prof_mc_n++; }
}

/* Item 10 follow-up: resolves every queued luma-DC Hadamard request in one
 * batched GPU dispatch, patching each intra macroblock's real DC value
 * into g_pending_coeffs[...][0] - must run before gpu_idct_batch (below in
 * flush_pending) reads that array, same ordering requirement resolve_mc_pending
 * already has against p->pred. */
static void resolve_lumadc_pending(void) {
    if (g_lumadc_pending_n == 0) return;
    static int dcbatch[LUMADC_BATCH_MAX][16];
    static int qmuls[LUMADC_BATCH_MAX];
    static int out[LUMADC_BATCH_MAX][16];
    for (int i = 0; i < g_lumadc_pending_n; i++) {
        memcpy(dcbatch[i], g_lumadc_pending[i].dc16, sizeof(dcbatch[i]));
        qmuls[i] = g_lumadc_pending[i].qmul;
    }
    gpu_lumadc_batch(dcbatch, qmuls, g_lumadc_pending_n, out);
    for (int i = 0; i < g_lumadc_pending_n; i++) {
        int base = g_lumadc_pending[i].pend_idx * 24;
        for (int blk = 0; blk < 16; blk++)
            g_pending_coeffs[base + blk][0] = out[i][luma_dc_perm[blk]] + 32;
    }
}

static int g_flush_count = 0;
static void flush_pending(void) {
    if (g_pending_n == 0) return;
    if (prof_on()) g_prof_flush_n++;
    if (getenv("DEBUG_BATCH")) {
        fprintf(stderr, "flush #%d: n=%d (MBs:", ++g_flush_count, g_pending_n);
        for (int m = 0; m < g_pending_n; m++) fprintf(stderr, " (%d,%d)", g_pending[m].mb_x, g_pending[m].mb_y);
        fprintf(stderr, ")\n");
    }
    /* Resolve every queued luma MC request (item 4, phase 4d) before the
     * residual add below reads p->pred - intra macroblocks never push any
     * (reconstruct_enqueue computes pred via CPU spatial prediction
     * directly), so this is a no-op whenever the batch is all-intra. */
    resolve_mc_pending();
    /* Same ordering requirement, item 10 follow-up: every intra macroblock's
     * real DC value must be patched into g_pending_coeffs[...][0] before
     * gpu_idct_batch reads it - a no-op whenever the batch is all-inter. */
    resolve_lumadc_pending();

    int total = g_pending_n * 24;
    static int batch_out[PENDING_MAX * 24][16];
    gpu_idct_batch(g_pending_coeffs, total, batch_out);

    for (int m = 0; m < g_pending_n; m++) {
        PendingMB *p = &g_pending[m];
        int base = m * 24;

        if (getenv("DEBUG_IDCT_MB") && p->mb_x == atoi(getenv("DEBUG_IDCT_MB")) &&
            p->mb_y == (getenv("DEBUG_IDCT_MB2") ? atoi(getenv("DEBUG_IDCT_MB2")) : -999)) {
            fprintf(stderr, "IDCT MB(%d,%d) frameno=%d:\n", p->mb_x, p->mb_y, g_x1900_debug_frameno);
            for (int blk = 0; blk < 16; blk++) {
                fprintf(stderr, "  blk%d in: ", blk);
                for (int c = 0; c < 16; c++) fprintf(stderr, "%d ", g_pending_coeffs[base+blk][c]);
                fprintf(stderr, "\n  blk%d out: ", blk);
                for (int c = 0; c < 16; c++) fprintf(stderr, "%d ", batch_out[base+blk][c]);
                fprintf(stderr, "\n");
            }
        }

        for (int blk = 0; blk < 16; blk++) {
            int *residual = batch_out[base + blk];
            int block_row = luma_blk_row[blk] * 4, block_col = luma_blk_col[blk] * 4;
            for (int r = 0; r < 4; r++)
                for (int c = 0; c < 4; c++)
                    p->dest_y[(block_row + r) * p->linesize + (block_col + c)] =
                        (uint8_t)clip255(p->pred[block_row + r][block_col + c] + residual[r * 4 + c]);
        }

        uint8_t *cdest[2] = { p->dest_cb, p->dest_cr };
        for (int plane = 0; plane < 2; plane++) {
            for (int k = 0; k < 4; k++) {
                int *residual = batch_out[base + 16 + plane * 4 + k];
                if (getenv("DEBUG_IDCT_MB") && p->mb_x == atoi(getenv("DEBUG_IDCT_MB")) &&
                    p->mb_y == (getenv("DEBUG_IDCT_MB2") ? atoi(getenv("DEBUG_IDCT_MB2")) : -999)) {
                    fprintf(stderr, "  cplane%d blk%d in: ", plane, k);
                    for (int c = 0; c < 16; c++) fprintf(stderr, "%d ", g_pending_coeffs[base+16+plane*4+k][c]);
                    fprintf(stderr, "\n  cplane%d blk%d out: ", plane, k);
                    for (int c = 0; c < 16; c++) fprintf(stderr, "%d ", residual[c]);
                    fprintf(stderr, "\n");
                }
                int block_row = chroma_blk_row[k] * 4, block_col = chroma_blk_col[k] * 4;
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 4; c++)
                        cdest[plane][(block_row + r) * p->uvlinesize + (block_col + c)] =
                            (uint8_t)clip255(p->cpred[plane][block_row + r][block_col + c] + residual[r * 4 + c]);
            }
        }
    }
    g_pending_n = 0;
    g_mc_pending_n = 0;
    g_lumadc_pending_n = 0;
}

/* Shared queue-push tail for any macroblock whose GPU IDCT is deferred
 * through the PendingMB queue - currently I16x16 intra (reconstruct_enqueue)
 * and P_16x16-with-residual inter (reconstruct_p16x16). `pred`/`cpred` are
 * this macroblock's already-computed prediction (spatial for intra, motion-
 * compensated for inter - the one thing that genuinely differs between the
 * two, so it stays the caller's job). `is_intra` selects between the two
 * real structural differences found by reading h264_mb_template.c directly
 * (see the plan's "P_16x16-with-residual: implementation plan" section):
 *   - intra: luma DC comes from `luma_dc_in`, the caller's raw (not yet
 *     Hadamard-transformed) DC values - resolve_lumadc_pending() batches the
 *     actual GPU transform at flush time (item 10 follow-up) and patches the
 *     result into this macroblock's slot before gpu_idct_batch reads it.
 *     I16x16 gets a dedicated DC transform FFmpeg doesn't run for any other
 *     mb_type, applied unconditionally - I16x16's own idct path is never
 *     itself wrapped in a cbp check.
 *   - inter: luma DC sits in-place at coeffs[blk*16+0] like any AC term (no
 *     separate Hadamard step), and the ENTIRE luma residual pass is gated on
 *     `cbp & 15` - a level above the individual nnz checks, exactly the
 *     shape of the already-known `cbp & 0x30` chroma gate below. Adding the
 *     rounding bias (+32, matching FFmpeg's own `block[0] += 1<<5` before
 *     its IDCT butterfly) unconditionally to whatever value ends up in
 *     bc[0] - real coefficient or the zero-initialized default - is safe
 *     either way: an all-zero 4x4 block plus this bias round-trips through
 *     gpu_idct_batch as an exact no-op (32>>6 == 0), which is also why the
 *     intra path already applies it unconditionally regardless of nnz.
 * Chroma reconstruction never distinguishes intra vs. inter in real FFmpeg
 * (h264_mb_template.c's chroma block doesn't check IS_INTRA at all), so it's
 * identical in both modes here too - only the chroma PREDICTION feeding
 * `cpred` differs (spatial vs. MC), and that's already been computed by the
 * caller before this function ever runs. */
static void enqueue_reconstruction(const X1900MbInfo *info, int pred[16][16],
                                    int cpred[2][8][8], int is_intra,
                                    const int *luma_dc_in, int luma_dc_qmul) {
    /* Item 9 frame-scale restructure (2026-08-28): removed the old
     * "row changed" backstop flush that used to live here - under the
     * previous per-row-only architecture it was a harmless, near-never-
     * triggered safety net (live_hook's own end-of-row flush already
     * guaranteed g_pending_n==0 by the time a new row's first macroblock
     * reached this function); under the new frame-scale architecture it
     * would trigger CONSTANTLY (the whole point is that g_pending_n now
     * legitimately spans many rows) and would silently defeat cross-row
     * batching entirely. The real correctness guarantee is now
     * live_hook's own checks: flush before any intra macroblock
     * (anywhere, any row) if backlog exists, and an unconditional flush
     * at end-of-frame - see live_hook's own comments.
     *
     * NOTE: the left-neighbor/top-neighbor-pending check does NOT live
     * here - it's live_hook's job (gated on IS_INTRA_MB of the CURRENT
     * macroblock, run before dispatch). A copy here would be redundant
     * for intra (live_hook's gate already ran, unconditionally, before
     * this function could be reached via reconstruct_enqueue) and
     * actively wrong for inter: an ungated check here would flush the
     * queue on every single P_16x16 enqueue whenever back-to-back inter
     * macroblocks are queued, defeating the entire point of gating it in
     * live_hook in the first place. Inter macroblocks never read spatial
     * context in this function's own body (their pred/cpred are already
     * fully computed via MC before this is called), so they never needed
     * this check to begin with. */
    if (g_pending_n >= PENDING_MAX)
        flush_pending(); /* safety cap - now a real, reachable cap on frame-scale content, not just theoretical */

    /* ---- chroma DC transform (CPU) for both planes - residual only;
     * prediction into cpred was already done by the caller. ---- */
    int base_blk[2] = { 16, 32 }; /* Cb AC blocks 16-19, Cr AC blocks 32-35 */
    int dc_xf[2][4];
    int chroma_has_residual = (info->cbp & 0x30) != 0;

    for (int plane = 0; plane < 2; plane++) {
        /* chroma DC: gather each AC block's own DC term (position 0),
         * transform, substitute back - same "DC lives at block[0]" idea
         * as luma, just a 2x2 grid and no GPU round-trip needed.
         *
         * Real FFmpeg gates its ENTIRE chroma-residual block - both
         * this DC transform AND every AC block below - on
         * `sl->cbp & 0x30`, a level above the individual
         * non_zero_count_cache checks (h264_mb_template.c). Unlike
         * luma_dc (sl->mb_luma_dc, a dedicated array refreshed every
         * macroblock), chroma DC values live in-place inside the
         * shared sl->mb block array - a general-purpose per-block
         * scratch space reused across macroblocks, so both the cbp gate
         * and the individual nnz checks below are required: cbp alone
         * can be set while a specific block's own nnz is 0 (real AC
         * coefficients elsewhere in the MB), and nnz can misleadingly
         * read nonzero from stale reused state that real reconstruction
         * never looks at once cbp already said "skip chroma entirely". */
        int dc_in[4]; for (int k=0;k<4;k++) dc_xf[plane][k] = 0;
        int chroma_dc_nnz = chroma_has_residual && info->nnz[plane == 0 ? 40 : 80]; /* scan8[CHROMA_DC_BLOCK_INDEX+plane] */
        if (chroma_dc_nnz) {
            for (int k = 0; k < 4; k++) dc_in[k] = info->coeffs[(base_blk[plane] + k) * 16 + 0];
            chroma_dc_transform(dc_in, info->chroma_dc_qmul[plane], dc_xf[plane]);
        }

        if (getenv("DEBUG_CHROMA_MB") && info->mb_x == atoi(getenv("DEBUG_CHROMA_MB")) &&
            info->mb_y == (getenv("DEBUG_CHROMA_MB2") ? atoi(getenv("DEBUG_CHROMA_MB2")) : -999)) {
            fprintf(stderr, "plane=%d is_intra=%d qmul=%d chroma_dc_nnz=%d dc_xf: %d %d %d %d\n",
                    plane, is_intra, info->chroma_dc_qmul[plane], chroma_dc_nnz,
                    dc_xf[plane][0], dc_xf[plane][1], dc_xf[plane][2], dc_xf[plane][3]);
        }
    }

    /* ---- gather all 24 blocks into this macroblock's slot in the
     * pending queue's shared coefficient array (no GPU call here - that
     * happens once per flush_pending(), not once per macroblock). ---- */
    int luma_has_residual = is_intra || (info->cbp & 15);
    if (getenv("DEBUG_P16_MB") && info->mb_x == atoi(getenv("DEBUG_P16_MB")) &&
        info->mb_y == (getenv("DEBUG_P16_MB2") ? atoi(getenv("DEBUG_P16_MB2")) : -999)) {
        fprintf(stderr, "P16 MB(%d,%d) frameno=%d: is_intra=%d cbp=0x%x cbp&15=%d mb_type=0x%x\n",
                info->mb_x, info->mb_y, g_x1900_debug_frameno, is_intra, info->cbp, info->cbp & 15, info->mb_type);
        fprintf(stderr, "  nnz(luma_ac_scan8): ");
        for (int blk = 0; blk < 16; blk++) fprintf(stderr, "%d ", info->nnz[luma_ac_scan8[blk]]);
        fprintf(stderr, "\n  coeffs[blk*16+0] (raw DC): ");
        for (int blk = 0; blk < 16; blk++) fprintf(stderr, "%d ", info->coeffs[blk * 16 + 0]);
        fprintf(stderr, "\n");
    }
    int base = g_pending_n * 24;
    for (int blk = 0; blk < 16; blk++) {
        int *bc = g_pending_coeffs[base + blk];
        for (int c = 0; c < 16; c++) bc[c] = 0;
        if (luma_has_residual && info->nnz[luma_ac_scan8[blk]])
            for (int c = 0; c < 16; c++) bc[c] = info->coeffs[blk * 16 + c];
        if (is_intra) {
            /* Placeholder - patched in place by resolve_lumadc_pending()
             * (item 10 follow-up) once this row's Hadamard batch resolves,
             * before flush_pending() calls gpu_idct_batch. See the
             * LumaDCReq push below, right after this block. */
        } else {
            bc[0] += 32; /* real DC (if the gated copy above ran) or 0 - either way, +32 alone round-trips as a no-op */
        }
    }
    for (int plane = 0; plane < 2; plane++) {
        for (int k = 0; k < 4; k++) {
            int *bc = g_pending_coeffs[base + 16 + plane * 4 + k];
            for (int c = 0; c < 16; c++) bc[c] = 0;
            if (chroma_has_residual && info->nnz[chroma_ac_scan8[plane][k]])
                for (int c = 0; c < 16; c++) bc[c] = info->coeffs[(base_blk[plane] + k) * 16 + c];
            bc[0] = dc_xf[plane][k] + 32;
        }
    }

    PendingMB *p = &g_pending[g_pending_n];
    p->mb_x = info->mb_x; p->mb_y = info->mb_y;
    p->dest_y = info->dest_y; p->dest_cb = info->dest_cb; p->dest_cr = info->dest_cr;
    p->linesize = info->linesize; p->uvlinesize = info->uvlinesize;
    memcpy(p->pred, pred, sizeof(p->pred));
    memcpy(p->cpred, cpred, sizeof(p->cpred));

    /* Attach this macroblock's own queued luma MC requests (if any - intra
     * macroblocks push none, see reconstruct_enqueue) to the slot they'll
     * land in, then hand the accumulator back for the next macroblock.
     * Resolved later, in flush_pending(), via resolve_mc_pending() -
     * item 4, phase 4d. */
    for (int i = 0; i < g_cur_mb_mc_n; i++) {
        if (g_mc_pending_n < MC_PENDING_MAX) {
            g_mc_pending[g_mc_pending_n] = g_cur_mb_mc[i];
            g_mc_pending[g_mc_pending_n].pend_idx = g_pending_n;
            g_mc_pending_n++;
        }
    }
    g_cur_mb_mc_n = 0;

    /* Item 10 follow-up: queue this intra macroblock's raw luma-DC input
     * for resolve_lumadc_pending() to batch-resolve at flush time, same
     * pend_idx-tagging pattern as the MC requests just above. */
    if (is_intra && g_lumadc_pending_n < PENDING_MAX) {
        LumaDCReq *r = &g_lumadc_pending[g_lumadc_pending_n++];
        r->pend_idx = g_pending_n;
        memcpy(r->dc16, luma_dc_in, sizeof(r->dc16));
        r->qmul = luma_dc_qmul;
    }

    g_pending_n++;

    /* End-of-row (including the frame's last row) is handled proactively
     * in live_hook, checked for every macroblock - see there. */
}

/* I16x16 intra: computes spatial prediction + the I16x16-specific luma-DC
 * Hadamard transform (a GPU call, not deferred - only the 24-block IDCT
 * itself is batched), then hands off to the shared tail above. */
static int reconstruct_enqueue(const X1900MbInfo *info) {
    /* ---- luma ---- */
    unsigned char left[16], top[16], topleft;
    uint8_t *dy = info->dest_y;
    int ls = info->linesize;
    /* LEFT context: same row, not yet deblocked - safe to read live. */
    for (int i = 0; i < 16; i++) left[i] = dy[-1 + i * ls];
    /* TOP context: item 9 frame-scale restructure (2026-08-28) - reads
     * directly from the live buffer instead of FFmpeg's preserved-pixel
     * cache (top_border_here/left). Safe because this function is now
     * ONLY ever reached for P-slice content (live_hook declines I-slices
     * from GPU takeover entirely - see live_hook's own comment on the
     * real interaction bug that made that necessary; B-slices were
     * already always declined), and P-slices always run with deblocking
     * POSTPONED for the whole frame while this hook is live (see
     * ff_x1900_hook_installed's comment in x1900_hook.h) - the row above
     * has NOT been deblocked yet, so the live buffer holds the same true
     * pre-deblock values the cache would have, matching LEFT context
     * above. Also correctly handles mb_x==0's "no left column" case being
     * moot here (this whole intra path is declined for mb_x==0/mb_y==0,
     * see live_hook). */
    for (int i = 0; i < 16; i++) top[i] = dy[-ls + i];
    topleft = dy[-ls - 1];

    if (getenv("DEBUG_LUMA_CTX") && info->mb_x == atoi(getenv("DEBUG_LUMA_CTX")) &&
        info->mb_y == (getenv("DEBUG_LUMA_CTX2") ? atoi(getenv("DEBUG_LUMA_CTX2")) : -999)) {
        fprintf(stderr, "LUMA CTX MB(%d,%d) frameno=%d mode=%d mb_type=0x%x cbp=0x%x\n",
                info->mb_x, info->mb_y, g_x1900_debug_frameno, info->intra16x16_pred_mode, info->mb_type, info->cbp);
        fprintf(stderr, "  left: "); for (int i=0;i<16;i++) fprintf(stderr,"%d ",left[i]); fprintf(stderr,"\n");
        fprintf(stderr, "  top:  "); for (int i=0;i<16;i++) fprintf(stderr,"%d ",top[i]); fprintf(stderr,"\n");
        fprintf(stderr, "  topleft: %d\n", topleft);
        fprintf(stderr, "  luma_dc: "); for (int i=0;i<16;i++) fprintf(stderr,"%d ",info->luma_dc[i]); fprintf(stderr,"\n");
        fprintf(stderr, "  luma_dc_qmul=%d\n", info->luma_dc_qmul);
    }

    int pred[16][16];
    pred16x16(info->intra16x16_pred_mode, left, top, topleft, pred);

    int luma_dc_in[16];
    for (int i = 0; i < 16; i++) luma_dc_in[i] = info->luma_dc[i];

    /* ---- chroma prediction (spatial - intra-specific) ---- */
    uint8_t *cdest[2] = { info->dest_cb, info->dest_cr };
    int uls = info->uvlinesize;
    int cpred[2][8][8];

    for (int plane = 0; plane < 2; plane++) {
        uint8_t *dc = cdest[plane];
        unsigned char cleft[8], ctop[8], ctopleft;
        for (int i = 0; i < 8; i++) cleft[i] = dc[-1 + i * uls];
        /* TOP context: same reasoning as luma above - this function is
         * only ever reached for P-slice content, always postponed. */
        for (int i = 0; i < 8; i++) ctop[i] = dc[-uls + i];
        ctopleft = dc[-uls - 1];

        pred8x8(info->chroma_pred_mode, cleft, ctop, ctopleft, cpred[plane]);

        if (getenv("DEBUG_CPRED_MB") && info->mb_x == atoi(getenv("DEBUG_CPRED_MB")) &&
            info->mb_y == (getenv("DEBUG_CPRED_MB2") ? atoi(getenv("DEBUG_CPRED_MB2")) : -999)) {
            fprintf(stderr, "CPRED MB(%d,%d) plane=%d mode=%d frameno=%d\n",
                    info->mb_x, info->mb_y, plane, info->chroma_pred_mode, g_x1900_debug_frameno);
            fprintf(stderr, "  cleft: "); for (int i=0;i<8;i++) fprintf(stderr,"%d ",cleft[i]); fprintf(stderr,"\n");
            fprintf(stderr, "  ctop:  "); for (int i=0;i<8;i++) fprintf(stderr,"%d ",ctop[i]); fprintf(stderr,"\n");
            fprintf(stderr, "  ctopleft: %d\n", ctopleft);
            fprintf(stderr, "  cpred row0: "); for (int i=0;i<8;i++) fprintf(stderr,"%d ",cpred[plane][0][i]); fprintf(stderr,"\n");
        }
    }

    enqueue_reconstruction(info, pred, cpred, 1, luma_dc_in, info->luma_dc_qmul);
    return 1;
}

/* Shared motion-compensated prediction for both P_Skip (writes immediately,
 * no residual) and P_16x16-with-residual (enqueues, residual added later at
 * flush_pending() time). Returns 0 for anything this project doesn't
 * handle: a B-slice macroblock, list-1/bi-predictive MVs, a missing/invalid
 * list-0 reference, or motion pointing too close to a frame edge for the
 * 6-tap filters' margin requirements - callers should decline (return 0) to
 * FFmpeg's own path in all of those cases, matching the original
 * reconstruct_skip's contract. */
static int compute_mc_pred(const X1900MbInfo *info, int pred[16][16], int cpred[2][8][8]) {
    /* Real bug found debugging P_16x16-with-residual: MB_TYPE_L1 alone
     * does NOT reliably catch every B-slice macroblock (see X1900MbInfo's
     * slice_type_nos comment - B_Direct/B_L0_16x16 partitions can use only
     * list-0 and genuinely never set MB_TYPE_L1, yet still aren't the
     * simple single-reference P-slice content this project's whole scope
     * is built for). Check the ENCLOSING SLICE's type directly instead of
     * trusting mb_type flags alone to imply "this is P-slice content". */
    if (info->slice_type_nos == AV_PICTURE_TYPE_B) return 0; /* B-slice - permanently out of scope, see the plan's item 11 */
    if (info->mb_type & MB_TYPE_L1) return 0; /* bi-predictive/list-1 - out of scope, see MB_TYPE_L1's comment */
    if (!info->ref_y) return 0; /* no valid list-0 reference - decline */

    int mv_x = info->mv_l0[0], mv_y = info->mv_l0[1]; /* quarter-luma-pel */
    int full_mx = mv_x + info->mb_x * 64;
    int full_my = mv_y + info->mb_y * 64;
    int h_phase = full_mx & 3, v_phase = full_my & 3;
    int pel_x = full_mx >> 2, pel_y = full_my >> 2;

    int frame_w = info->mb_width * 16, frame_h = info->mb_height * 16;
    /* 6-tap luma filter needs 2px margin left/above, 3px right/below. */
    if (pel_x - 2 < 0 || pel_y - 2 < 0 || pel_x + 18 >= frame_w || pel_y + 18 >= frame_h)
        return 0; /* motion points too close to a frame edge - decline, let FFmpeg's own emulated-edge path handle it */

    int cfull_mx = full_mx, cfull_my = full_my; /* chroma shares the same quarter-luma-pel value */
    int cpel_x = cfull_mx >> 3, cpel_y = cfull_my >> 3;
    int cphase_x = cfull_mx & 7, cphase_y = cfull_my & 7;
    int cw = info->mb_width * 8, ch = info->mb_height * 8;
    if (cpel_x < 0 || cpel_y < 0 || cpel_x + 9 >= cw || cpel_y + 9 >= ch)
        return 0;

    if (getenv("DEBUG_SKIP_MB") && info->mb_x == atoi(getenv("DEBUG_SKIP_MB")) && info->mb_y == atoi(getenv("DEBUG_SKIP_MB2"))) {
        fprintf(stderr, "MC MB(%d,%d): mv=(%d,%d) full=(%d,%d) phase=(%d,%d) pel=(%d,%d) ref_idx_linesize=%d\n",
                info->mb_x, info->mb_y, mv_x, mv_y, full_mx, full_my, h_phase, v_phase, pel_x, pel_y, info->ref_linesize);
        fprintf(stderr, "ref_y at (pel_x,pel_y) row0: ");
        for (int c = 0; c < 16; c++) fprintf(stderr, "%d ", info->ref_y[pel_y*info->ref_linesize+pel_x+c]);
        fprintf(stderr, "\n");
    }

    /* Item 4, phase 4d: luma MC is no longer computed here on the CPU -
     * push a request and let resolve_mc_pending() fill pred[][] via a
     * batched GPU dispatch at flush_pending() time instead (see that
     * section's comment). `pred` itself goes unused in this function now;
     * still accepted as a parameter since callers pass a real pred[16][16]
     * either way (intra callers still fill it directly, elsewhere). */
    g_mc_frame_w = frame_w; g_mc_frame_h = frame_h;
    push_mc_req(0, 0, 16, 16, pel_x, pel_y, h_phase, v_phase, info->ref_y, info->ref_linesize);

    unsigned char cb[8][8], cr[8][8];
    mc_chroma8(cb, info->ref_cb + cpel_y * info->ref_uvlinesize + cpel_x, info->ref_uvlinesize, cphase_x, cphase_y);
    mc_chroma8(cr, info->ref_cr + cpel_y * info->ref_uvlinesize + cpel_x, info->ref_uvlinesize, cphase_x, cphase_y);
    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
        cpred[0][r][c] = cb[r][c];
        cpred[1][r][c] = cr[r][c];
    }

    return 1;
}

/* P_Skip macroblock reconstruction: no residual at all (skip is defined
 * as pure motion-compensated copy - the encoder chose it specifically
 * because the PREDICTED motion vector, already computed by FFmpeg's own
 * entropy decode and sitting in mv_cache exactly like any other
 * macroblock's MV, was good enough with zero coded difference). Unlike
 * the intra path, this has NO same-frame spatial dependency at all -
 * everything is read from the reference (previous) frame, already
 * fully decoded - so it's written immediately, synchronously, with none
 * of the pending-queue machinery intra macroblocks needed. This is the
 * concrete case that validates last session's finding: inter
 * macroblocks don't have the raster dependency chain that made
 * cross-macroblock batching a dead end for intra content. */
static int reconstruct_skip(const X1900MbInfo *info) {
    /* Item 4, phase 4d: now that luma MC is a real GPU dispatch (not free
     * CPU arithmetic), a skip macroblock needs the same cross-macroblock
     * batching everything else gets, or it pays a full per-macroblock GPU
     * round trip alone - so this now enqueues through the same PendingMB
     * queue as P_16x16 instead of writing immediately. Correctness is
     * unchanged: with no coded residual (cbp implicitly 0 for a skip
     * macroblock), enqueue_reconstruction's luma_has_residual/
     * chroma_has_residual gates are both false, so every one of its 24
     * blocks' coefficients are all-zero - already an established, verified
     * no-op through gpu_idct_batch (see enqueue_reconstruction's own
     * comment on the DC "+32" bias round-tripping to nothing). */
    int pred[16][16], cpred[2][8][8];
    if (!compute_mc_pred(info, pred, cpred)) return 0;
    enqueue_reconstruction(info, pred, cpred, 0, NULL, 0);
    return 1;
}

/* P_16x16-with-residual: identical motion-compensated prediction to
 * reconstruct_skip (see compute_mc_pred) plus a real coded residual.
 * Deferred through the same PendingMB queue I16x16 uses purely because GPU
 * IDCT dispatch is batched there - NOT because of any same-frame spatial
 * dependency (inter macroblocks have none; see the IS_INTRA_MB gate on the
 * left-neighbor flush check in live_hook, which this relies on to actually
 * deliver batching rather than collapsing to size-1 the way intra content
 * does). */
static int reconstruct_p16x16(const X1900MbInfo *info) {
    /* 8x8 transform mode uses a different-sized IDCT (h264_idct8_add4,
     * not this project's gpu_idct_batch's 4x4-per-block assumption) -
     * see MB_TYPE_8x8DCT's comment. reconstruct_skip doesn't need this
     * check (no residual/IDCT at all for a skip macroblock), only the
     * residual-carrying inter path does. Found as a real bug while
     * implementing item 3 (P_16x8/P_8x16) - a latent gap in this
     * already-shipped P_16x16 path too, fixed here alongside it. */
    if (info->mb_type & MB_TYPE_8x8DCT) return 0;
    int pred[16][16], cpred[2][8][8];
    if (!compute_mc_pred(info, pred, cpred)) return 0;
    enqueue_reconstruction(info, pred, cpred, 0, NULL, 0);
    return 1;
}

/* Per-partition motion compensation for P_16x8/P_8x16 (item 3 design
 * pass, phase 3a) - writes one partition's WxH luma block into pred[16]
 * [16] at (x_off,y_off) and the corresponding (W/2)x(H/2) chroma block
 * into cpred[2][8][8] at (x_off/2,y_off/2). `n` is the real z-order
 * block index for this partition's mv/ref (n=0/8 for 16x8's top/bottom
 * halves, n=0/4 for 8x16's left/right halves - see the plan's item-3
 * design pass, verified against real hl_motion/mc_part). Mirrors
 * compute_mc_pred's structure exactly, generalized to an arbitrary
 * sub-rectangle instead of always the whole 16x16/8x8 macroblock.
 *
 * v1 multi-reference restriction (a real, explicitly undecided scope
 * question - see the plan): declines outright if this partition's own
 * reference index differs from the macroblock's shared reference
 * (info->ref_y, built from scan8[0]'s ref_idx) - only macroblocks where
 * every partition agrees on one reference are handled; true per-
 * partition multi-reference is a real follow-up, not attempted here. */
static int compute_mc_part(const X1900MbInfo *info, int n, int x_off, int y_off,
                            int w, int h, int pred[16][16], int cpred[2][8][8]) {
    int idx = luma_ac_scan8[n];
    if (info->ref_l0_cache[idx] != info->ref_l0[0]) return 0; /* disagreeing reference - v1 declines, see plan */

    int mv_x = info->mv_l0_cache[2*idx + 0], mv_y = info->mv_l0_cache[2*idx + 1];
    int full_mx = mv_x + (info->mb_x * 16 + x_off) * 4;
    int full_my = mv_y + (info->mb_y * 16 + y_off) * 4;
    int h_phase = full_mx & 3, v_phase = full_my & 3;
    int pel_x = full_mx >> 2, pel_y = full_my >> 2;

    int frame_w = info->mb_width * 16, frame_h = info->mb_height * 16;
    if (pel_x - 2 < 0 || pel_y - 2 < 0 || pel_x + w + 2 >= frame_w || pel_y + h + 2 >= frame_h)
        return 0;

    /* Item 4, phase 4d: same deferral as compute_mc_pred above - queue this
     * partition's luma MC instead of computing it here on the CPU. */
    g_mc_frame_w = frame_w; g_mc_frame_h = frame_h;
    push_mc_req(x_off, y_off, w, h, pel_x, pel_y, h_phase, v_phase, info->ref_y, info->ref_linesize);

    if (getenv("DEBUG_MC_PART") && info->mb_x == atoi(getenv("DEBUG_MC_PART")) &&
        info->mb_y == (getenv("DEBUG_MC_PART2") ? atoi(getenv("DEBUG_MC_PART2")) : -999)) {
        fprintf(stderr, "compute_mc_part MB(%d,%d) n=%d x_off=%d y_off=%d w=%d h=%d: "
                "pel=(%d,%d) phase=(%d,%d) ref_linesize=%d\n",
                info->mb_x, info->mb_y, n, x_off, y_off, w, h, pel_x, pel_y, h_phase, v_phase, info->ref_linesize);
        const unsigned char *refbase = info->ref_y + pel_y * info->ref_linesize + pel_x;
        fprintf(stderr, "  ref row0 (cols -2..%d): ", w + 2);
        for (int c = -2; c <= w + 2; c++) fprintf(stderr, "%d ", refbase[c]);
        fprintf(stderr, "\n  ref row1: ");
        for (int c = -2; c <= w + 2; c++) fprintf(stderr, "%d ", refbase[info->ref_linesize + c]);
        fprintf(stderr, "\n  (pred luma no longer computed here - now resolved later via GPU MC, see resolve_mc_pending)\n");
    }

    int cw = w / 2, ch = h / 2, cx_off = x_off / 2, cy_off = y_off / 2;
    int cpel_x = full_mx >> 3, cpel_y = full_my >> 3;
    int cphase_x = full_mx & 7, cphase_y = full_my & 7;
    int cframe_w = info->mb_width * 8, cframe_h = info->mb_height * 8;
    if (cpel_x < 0 || cpel_y < 0 || cpel_x + cw + 1 >= cframe_w || cpel_y + ch + 1 >= cframe_h)
        return 0;

    unsigned char cb[8][8], cr[8][8];
    mc_chroma_wh(cb, info->ref_cb + cpel_y * info->ref_uvlinesize + cpel_x, info->ref_uvlinesize, cw, ch, cphase_x, cphase_y);
    mc_chroma_wh(cr, info->ref_cr + cpel_y * info->ref_uvlinesize + cpel_x, info->ref_uvlinesize, cw, ch, cphase_x, cphase_y);
    for (int r = 0; r < ch; r++) for (int c = 0; c < cw; c++) {
        cpred[0][cy_off+r][cx_off+c] = cb[r][c];
        cpred[1][cy_off+r][cx_off+c] = cr[r][c];
    }

    return 1;
}

/* P_16x8 / P_8x16 (item 3 design pass, phase 3a): two independent MC
 * partitions instead of one, each with its own mv/ref via
 * compute_mc_part. Residual/IDCT is completely UNCHANGED from P_16x16 -
 * MC partition shape and the 4x4 residual transform are orthogonal in
 * H.264 (verified directly against hl_decode_mb_idct_luma's cbp&15 gate,
 * see the plan) - so this reuses enqueue_reconstruction exactly as-is
 * once pred/cpred are fully populated, same as reconstruct_p16x16. */
static int reconstruct_p16x8_8x16(const X1900MbInfo *info) {
    if (info->slice_type_nos == AV_PICTURE_TYPE_B) return 0; /* B-slice - permanently out of scope */
    if (info->mb_type & MB_TYPE_L1) return 0; /* bi-predictive/list-1 - out of scope */
    if (info->mb_type & MB_TYPE_8x8DCT) return 0; /* 8x8 transform mode - wrong-size IDCT otherwise, see MB_TYPE_8x8DCT's comment */
    if (!info->ref_y) return 0; /* no valid list-0 reference */

    int pred[16][16], cpred[2][8][8];
    int ok;
    int is16x8 = (info->mb_type & MB_TYPE_16x8) != 0;
    if (is16x8) {
        ok = compute_mc_part(info, 0, 0, 0, 16, 8, pred, cpred) &&
             compute_mc_part(info, 8, 0, 8, 16, 8, pred, cpred);
    } else { /* MB_TYPE_8x16 */
        ok = compute_mc_part(info, 0, 0, 0, 8, 16, pred, cpred) &&
             compute_mc_part(info, 4, 8, 0, 8, 16, pred, cpred);
    }

    if (getenv("DEBUG_P16X8_MB") && info->mb_x == atoi(getenv("DEBUG_P16X8_MB")) &&
        info->mb_y == (getenv("DEBUG_P16X8_MB2") ? atoi(getenv("DEBUG_P16X8_MB2")) : -999)) {
        fprintf(stderr, "P16X8 MB(%d,%d) frameno=%d: is16x8=%d ok=%d mb_type=0x%x\n",
                info->mb_x, info->mb_y, g_x1900_debug_frameno, is16x8, ok, info->mb_type);
        int n0 = is16x8 ? 0 : 0, n1 = is16x8 ? 8 : 4;
        int idx0 = luma_ac_scan8[n0], idx1 = luma_ac_scan8[n1];
        fprintf(stderr, "  part0 n=%d idx=%d mv=(%d,%d) ref=%d\n", n0, idx0,
                info->mv_l0_cache[2*idx0], info->mv_l0_cache[2*idx0+1], info->ref_l0_cache[idx0]);
        fprintf(stderr, "  part1 n=%d idx=%d mv=(%d,%d) ref=%d\n", n1, idx1,
                info->mv_l0_cache[2*idx1], info->mv_l0_cache[2*idx1+1], info->ref_l0_cache[idx1]);
        fprintf(stderr, "  whole-mb ref_l0=%d (scan8[0]=%d)\n", info->ref_l0[0], luma_ac_scan8[0]);
    }

    if (!ok) return 0;

    enqueue_reconstruction(info, pred, cpred, 0, NULL, 0);
    return 1;
}

/* P_8x8, phases 3b+3c combined: every quadrant's own sub_mb_type decides
 * its own granularity independently (a real macroblock can freely mix
 * shapes across its four quadrants - nothing forces them to agree). A
 * quadrant is a square region at real pixel offset (qx,qy) = (0,0)/
 * (8,0)/(0,8)/(8,8) for i=0..3, base mv/ref index n=4*i (verified
 * against real hl_motion, h264_mc_template.c):
 *   - IS_SUB_8X8 (phase 3b): one 8x8 MC call at (qx,qy), index n.
 *   - IS_SUB_8X4 (phase 3c): two 8x4 halves, top at (qx,qy) index n,
 *     bottom at (qx,qy+4) index n+2 - n/n+2 are vertically adjacent in
 *     the z-order block numbering (same one this project's IDCT/luma-DC
 *     code already uses), matching real hl_motion's own n/n+2 pairing.
 *   - IS_SUB_4X8 (phase 3c): two 4x8 halves, left at (qx,qy) index n,
 *     right at (qx+4,qy) index n+1 (horizontally adjacent, n/n+1).
 *   - IS_SUB_4X4 (phase 3c): four 4x4 corners, index n+j for j=0..3 at
 *     (qx,qy)/(qx+4,qy)/(qx,qy+4)/(qx+4,qy+4) - same TL/TR/BL/BR z-order
 *     pattern as the quadrants themselves, one level down.
 * Every case reuses compute_mc_part exactly as-is (down to 4x4 - the
 * WxH-generalized MC helpers handle it, no new math). Real quadrant
 * dimensions/offsets/index math verified in the plan's item-3 design
 * pass. Same B-slice/list-1/8x8dct/reference-agreement decline reasons
 * as P_16x8/P_8x16 (compute_mc_part's own ref-agreement check runs once
 * per real MC partition, however fine). */
static int reconstruct_p8x8(const X1900MbInfo *info) {
    if (info->slice_type_nos == AV_PICTURE_TYPE_B) return 0; /* B-slice - permanently out of scope */
    if (info->mb_type & MB_TYPE_L1) return 0; /* bi-predictive/list-1 - out of scope */
    if (info->mb_type & MB_TYPE_8x8DCT) return 0; /* 8x8 transform mode - wrong-size IDCT otherwise */
    if (!info->ref_y) return 0; /* no valid list-0 reference */

    int pred[16][16], cpred[2][8][8];
    int ok = 1;
    for (int i = 0; i < 4 && ok; i++) {
        int n = 4 * i;
        int qx = (i & 1) ? 8 : 0;
        int qy = (i & 2) ? 8 : 0;
        int smt = info->sub_mb_type[i];
        if (IS_SUB_8X8(smt)) {
            ok = compute_mc_part(info, n, qx, qy, 8, 8, pred, cpred);
        } else if (IS_SUB_8X4(smt)) {
            ok = compute_mc_part(info, n,     qx, qy,     8, 4, pred, cpred) &&
                 compute_mc_part(info, n + 2, qx, qy + 4, 8, 4, pred, cpred);
        } else if (IS_SUB_4X8(smt)) {
            ok = compute_mc_part(info, n,     qx,     qy, 4, 8, pred, cpred) &&
                 compute_mc_part(info, n + 1, qx + 4, qy, 4, 8, pred, cpred);
        } else { /* IS_SUB_4X4 */
            ok = compute_mc_part(info, n,     qx,     qy,     4, 4, pred, cpred) &&
                 compute_mc_part(info, n + 1, qx + 4, qy,     4, 4, pred, cpred) &&
                 compute_mc_part(info, n + 2, qx,     qy + 4, 4, 4, pred, cpred) &&
                 compute_mc_part(info, n + 3, qx + 4, qy + 4, 4, 4, pred, cpred);
        }
    }
    if (!ok) return 0;

    enqueue_reconstruction(info, pred, cpred, 0, NULL, 0);
    return 1;
}

static int g_live = 0;      /* whether the hook is allowed to take over */
static int g_took_over = 0; /* count of MBs the hook actually reconstructed */
static int g_declined = 0;
static int g_skip_took_over = 0, g_skip_declined = 0;
static int g_p16_took_over = 0, g_p16_declined = 0;
static int g_p16x8_took_over = 0, g_p16x8_declined = 0;
static int g_p8x8_took_over = 0, g_p8x8_declined = 0;
static int g_first_mb_x = -1, g_first_mb_y = -1;
#define MAX_TRACK 4096
static int g_track_x[MAX_TRACK], g_track_y[MAX_TRACK], g_track_mode[MAX_TRACK], g_track_n = 0;
static int g_skip_track_x[MAX_TRACK], g_skip_track_y[MAX_TRACK], g_skip_track_n = 0;
static int g_p16_track_x[MAX_TRACK], g_p16_track_y[MAX_TRACK], g_p16_track_frame[MAX_TRACK], g_p16_track_n = 0;
static int g_p16x8_track_x[MAX_TRACK], g_p16x8_track_y[MAX_TRACK], g_p16x8_track_n = 0;
static int g_p8x8_track_x[MAX_TRACK], g_p8x8_track_y[MAX_TRACK], g_p8x8_track_n = 0;

static int live_hook(const X1900MbInfo *info, void *ud) {
    (void)ud;
    int result = 0;
    if (!g_live) return 0; /* hook not live at all - nothing to flush either */
    /* Reset this macroblock's own MC-request accumulator unconditionally,
     * not just on a successful enqueue - a macroblock that pushes some
     * partitions (compute_mc_part succeeding for one quadrant) then
     * declines overall (another quadrant failing a margin/ref check) must
     * not leak those entries into whichever later macroblock enqueues
     * next. See push_mc_req/compute_mc_part/compute_mc_pred and
     * enqueue_reconstruction's own drain of this same accumulator. */
    g_cur_mb_mc_n = 0;
    /* Reference-texture cache (see reftex_lookup_or_upload) is keyed by
     * raw AVFrame plane pointer, valid only for as long as that pointer's
     * PIXEL CONTENT is guaranteed stable. That's true for the whole of the
     * CURRENT frame's own decode (FFmpeg guarantees a reference frame
     * can't change while something is still decoding against it), but NOT
     * across to the next frame within the same decode_to_frame() run -
     * FFmpeg's frame buffer pool can and does recycle a released buffer's
     * address for an entirely different later frame's pixels once nothing
     * still needs it as a reference. Invalidating once per decode_to_frame
     * call (as this project's other counters already do) is therefore not
     * enough for a run that decodes several real frames back to back (any
     * TARGET_FRAME beyond the very first) - invalidate on every new
     * frame's first macroblock instead, which still preserves the cache's
     * real benefit (every row-flush within one frame shares it). */
    if (info->mb_x == 0 && info->mb_y == 0) reftex_cache_reset();
    /* g_x1900_debug_frameno is incremented once per real macroblock-0 in
     * h264_mb.c's ff_h264_hl_decode_mb (called unconditionally for every
     * macroblock regardless of whether any hook is installed) - do not
     * also increment it here, that would double-count during this (live)
     * pass specifically since ff_h264_hl_decode_mb always runs first. */

    /* A DECLINED macroblock (the vast majority - FFmpeg's own C code
     * reconstructs it normally) reads ITS OWN neighbor context straight
     * from the live buffer, with no awareness of our pending queue at
     * all. If its LEFT or TOP neighbor is one of our own macroblocks
     * still sitting unflushed in the queue, FFmpeg would read wrong
     * (placeholder/unwritten) pixels - this dependency check has to run
     * for every macroblock, not just ones we personally take over.
     *
     * Item 9 frame-scale restructure (2026-08-28): the pending queue can
     * now span MANY rows (deblocking is postponed for the whole frame,
     * see ff_x1900_hook_installed's comment), so a TOP-neighbor
     * dependency is real and no longer automatically ruled out the way
     * it was when every row flushed unconditionally.
     *
     * A first version of this check simply flushed the WHOLE backlog on
     * ANY pending content whenever an intra macroblock was seen - correct,
     * but measured to cost real batching: P-slices in real content still
     * carry scattered intra-refresh macroblocks (not just I-slices), and
     * "flush everything" on every one of those left resolve_mc_pending's
     * own call count almost unchanged from before this restructure
     * (492 vs. 501 calls on the same 40-frame profiled run) - most of the
     * intended cross-row batching benefit was being thrown away by this
     * check's own over-eagerness. Narrowed to the PRECISE dependency:
     * flush only if (a) the OLDEST pending entry is from an earlier row
     * than this macroblock (its top-neighbor's row might still be
     * unflushed - pending entries are always enqueued in raster order, so
     * g_pending[0] is always the oldest/earliest), or (b) this
     * macroblock's specific left neighbor (mb_x-1, same row) is itself
     * still pending. Both real, both necessary; nothing else is. */
    if (IS_INTRA_MB(info->mb_type) && g_pending_n > 0) {
        int need_flush = (g_pending[0].mb_y < info->mb_y);
        if (!need_flush)
            for (int m = 0; m < g_pending_n; m++)
                if (g_pending[m].mb_x == info->mb_x - 1 && g_pending[m].mb_y == info->mb_y) {
                    need_flush = 1;
                    break;
                }
        if (need_flush) flush_pending();
    }

    if (info->mb_type & MB_TYPE_SKIP) {
        /* Inter path - no same-frame dependency, but (since item 4, phase
         * 4d) DOES use the pending queue now, purely for GPU MC batching -
         * see reconstruct_skip's own comment. */
        result = reconstruct_skip(info);
        if (result) {
            g_skip_took_over++;
            if (g_skip_track_n < MAX_TRACK) {
                g_skip_track_x[g_skip_track_n] = info->mb_x;
                g_skip_track_y[g_skip_track_n] = info->mb_y;
                g_skip_track_n++;
            }
        } else g_skip_declined++;
    } else if (info->mb_type & MB_TYPE_16x16) {
        /* P_16x16-with-residual - same MC as skip, plus a real residual,
         * deferred via the pending queue (see reconstruct_p16x16). Any
         * decline (list-1, no reference, edge margin) is handled inside
         * compute_mc_pred, matching reconstruct_skip's own pattern. */
        result = reconstruct_p16x16(info);
        if (result) {
            g_p16_took_over++;
            if (g_p16_track_n < MAX_TRACK) {
                g_p16_track_x[g_p16_track_n] = info->mb_x;
                g_p16_track_y[g_p16_track_n] = info->mb_y;
                g_p16_track_frame[g_p16_track_n] = g_x1900_debug_frameno;
                g_p16_track_n++;
            }
        } else g_p16_declined++;
    } else if (info->mb_type & (MB_TYPE_16x8 | MB_TYPE_8x16)) {
        /* P_16x8/P_8x16 (item 3, phase 3a) - two independent MC
         * partitions, see reconstruct_p16x8_8x16. Same decline reasons
         * as P_16x16 plus a new one: the two partitions disagreeing on
         * which reference frame to use (see compute_mc_part's comment). */
        result = reconstruct_p16x8_8x16(info);
        if (result) {
            g_p16x8_took_over++;
            if (g_p16x8_track_n < MAX_TRACK) {
                g_p16x8_track_x[g_p16x8_track_n] = info->mb_x;
                g_p16x8_track_y[g_p16x8_track_n] = info->mb_y;
                g_p16x8_track_n++;
            }
        } else g_p16x8_declined++;
    } else if (info->mb_type & MB_TYPE_8x8) {
        /* P_8x8 (item 3, phases 3b+3c) - each quadrant's own sub_mb_type
         * decides its own granularity (whole-8x8/8x4/4x8/4x4), see
         * reconstruct_p8x8. */
        result = reconstruct_p8x8(info);
        if (result) {
            g_p8x8_took_over++;
            if (g_p8x8_track_n < MAX_TRACK) {
                g_p8x8_track_x[g_p8x8_track_n] = info->mb_x;
                g_p8x8_track_y[g_p8x8_track_n] = info->mb_y;
                g_p8x8_track_n++;
            }
        } else g_p8x8_declined++;
    } else if (!(info->mb_type & MB_TYPE_INTRA16x16)) {
        g_declined++;
    } else if (info->slice_type_nos == AV_PICTURE_TYPE_I) {
        /* Item 9 frame-scale restructure (2026-08-28): I16x16 GPU takeover
         * disabled for I-slices, a real, deliberate scope narrowing found
         * necessary during this restructure - NOT a pre-existing
         * restriction (I16x16 takeover in I-slices was proven byte-exact
         * and has worked since early in this project). Root cause: a
         * genuine, confirmed (isolated via a direct A/B test - reconfirmed
         * byte-exact at 0.000% the instant I-slice takeover is disabled,
         * REGARDLESS of whether deblocking postponement is also active)
         * but NOT FULLY TRACED interaction where taking over an I16x16
         * macroblock somewhere in an I-slice corrupts something that
         * later corrupts a small number of DECLINED I4x4/I8x8
         * macroblocks elsewhere in the SAME frame (concentrated at the
         * frame's right edge in this content, 2 macroblocks/frame,
         * ~0.2% of pixels) - confirmed NOT about deblocking/postponement
         * timing (persists with postponement fully disabled for I-slices
         * too) and NOT about this project's own flush-trigger timing
         * (the same "flush before any intra macroblock" check already
         * covers declined content). Suspected but unconfirmed: some
         * FFmpeg-internal per-slice state (e.g. non_zero_count_cache
         * staleness, matching this project's own earlier "sl->mb reused-
         * scratch-buffer" bug class) that our hook's early return skips
         * updating in a way real, non-hooked reconstruction implicitly
         * relies on for a LATER macroblock's own reconstruction - not
         * confirmed further given time spent already; a real candidate
         * for future investigation, not a GPU/driver quirk (purely
         * FFmpeg-internal). Accepted as the fix rather than continuing to
         * chase the exact mechanism: I-slice GPU reconstruction was never
         * the source of real GPU cost (item 10's dominant cost is P-slice
         * MC dispatch) and never batched past size 1 anyway (the
         * left/top-neighbor-pending flush check already collapsed intra
         * content to batch-size-1, independent of this restructure) - so
         * this costs nothing toward the real goal while fully restoring
         * correctness. P/B-slice content (P16x16/P16x8/P8x16/P8x8/Skip
         * above) is unaffected and is where this restructure's real
         * frame-scale batching benefit lives. */
        g_declined++;
    } else if (info->mb_x == 0 || info->mb_y == 0) {
        /* Edge macroblocks need real edge-availability handling
         * (DC-with-missing-neighbor variants) not implemented here -
         * decline and let FFmpeg's normal path handle those, same
         * restriction as gpu-full-intra16-test. */
        g_declined++;
    } else {
        if (g_took_over == 0) { g_first_mb_x = info->mb_x; g_first_mb_y = info->mb_y; }
        if (g_track_n < MAX_TRACK) {
            g_track_x[g_track_n] = info->mb_x; g_track_y[g_track_n] = info->mb_y;
            g_track_mode[g_track_n] = info->chroma_pred_mode; g_track_n++;
        }
        g_took_over++;
        result = reconstruct_enqueue(info);
    }

    /* Item 9 frame-scale restructure (2026-08-28): the old unconditional
     * end-of-ROW flush is gone - deferring across rows is now safe (see
     * ff_x1900_hook_installed's comment), and that per-row flush was
     * exactly the thing standing between this project and real
     * cross-row/frame-scale GPU dispatch batching. What remains
     * mandatory: the end-of-FRAME flush, checked for EVERY macroblock
     * (declined or not, matching the original comment's own reasoning for
     * why this can't be a reactive "did mb_y change" check - by the time
     * that would fire, on the NEXT frame's first macroblock, FFmpeg's own
     * loop_filter catch-up for THIS frame - triggered right after
     * decode_slice() returns, see h264_slice.c - would already have run
     * against an incomplete buffer). Flushing here guarantees every
     * pending macroblock's pixels are written before this hook call
     * returns control past the frame's last macroblock. */
    if (info->mb_x == info->mb_width - 1 && info->mb_y == info->mb_height - 1)
        flush_pending();

    return result;
}

/* ================= decode-and-compare harness ================= */

static AVFrame *decode_to_frame(Mp4Movie *mov, unsigned char *avcc, int alen, int hook_live, int target_frame) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(alen + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)alen); ctx->extradata_size = alen;
    ctx->thread_count = 1;
    ctx->thread_type = 0;
    if (getenv("NO_DEBLOCK")) ctx->skip_loop_filter = AVDISCARD_ALL;
    avcodec_open2(ctx, codec, NULL);

    g_live = hook_live;
    ff_x1900_set_postpone_wanted(hook_live); /* item 9: keep the ref pass on the normal, unpostponed path - see x1900_hook.h's comment */
    g_took_over = 0; g_declined = 0; g_skip_took_over = 0; g_skip_declined = 0;
    g_p16_took_over = 0; g_p16_declined = 0;
    g_p16x8_took_over = 0; g_p16x8_declined = 0; g_p16x8_track_n = 0;
    g_p8x8_took_over = 0; g_p8x8_declined = 0; g_p8x8_track_n = 0; g_x1900_debug_frameno = -1;
    g_pending_n = 0; g_mc_pending_n = 0; g_cur_mb_mc_n = 0; g_lumadc_pending_n = 0;
    g_prof_lumadc_ms = g_prof_idct_ms = g_prof_mc_ms = 0;
    g_prof_lumadc_cpu_ms = g_prof_idct_cpu_ms = g_prof_mc_cpu_ms = 0;
    g_prof_sp_pack_ms = g_prof_sp_pack_cpu_ms = g_prof_sp_upload_ms = g_prof_sp_upload_cpu_ms = 0;
    g_prof_sp_draw_ms = g_prof_sp_draw_cpu_ms = g_prof_sp_read_ms = g_prof_sp_read_cpu_ms = 0; g_prof_sp_chunks = 0;
    g_prof_diag_ms = g_prof_diag_cpu_ms = 0; g_prof_diag_n = g_prof_diag_chunks = 0;
    g_prof_ib_pack_ms = g_prof_ib_pack_cpu_ms = g_prof_ib_upload_ms = g_prof_ib_upload_cpu_ms = 0;
    g_prof_ib_draw_ms = g_prof_ib_draw_cpu_ms = g_prof_ib_read_ms = g_prof_ib_read_cpu_ms = 0;
    g_prof_reftex_hit_ms = g_prof_reftex_hit_cpu_ms = g_prof_reftex_miss_ms = g_prof_reftex_miss_cpu_ms = 0;
    g_prof_reftex_hit_n = g_prof_reftex_miss_n = 0;
    g_prof_lumadc_n = g_prof_idct_n = g_prof_mc_n = g_prof_flush_n = 0;
    reftex_cache_reset();
    ff_x1900_set_mb_hook(live_hook, NULL);

    /* Decode until frame_idx (0-based, DISPLAY/OUTPUT order - this is what
     * avcodec_receive_frame hands back, and what target_frame actually
     * selects) reaches target_frame. Real finding, made while debugging
     * P_16x16-with-residual: this test clip DOES have B-frames (an earlier
     * comment here claiming otherwise was wrong) - decode order and
     * display order genuinely diverge (confirmed via a real POC trace:
     * decode order 0,1,2,3 has POC 65536,65544,65540,65538, i.e. display
     * order is 0,3,2,1). g_x1900_debug_frameno (shared with h264_mb.c's own
     * ground-truth trace) counts in DECODE order, since that's the order
     * macroblocks actually stream through the hook - it is NOT the same
     * number as target_frame/frame_idx once B-frames are involved. Capture
     * its value at the moment the target DISPLAY-order frame is found, so
     * debug tooling that needs to correlate a tracked macroblock against
     * THIS specific decoded frame (not some other frame that happens to
     * share the same mb_x/mb_y) filters on the right value. */
    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    AVFrame *result = NULL;
    int frame_idx = -1;
    for (uint32_t i = 0; i < mov->sample_count && !result; i++) {
        Mp4Sample *s = &mov->samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov->file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            frame_idx++;
            if (frame_idx == target_frame && !result) {
                result = av_frame_alloc();
                av_frame_ref(result, frame);
                g_x1900_debug_captured_frameno = g_x1900_debug_frameno;
            }
            av_frame_unref(frame);
            if (result) break;
        }
    }
    /* Safety net: the last-row check inside reconstruct_enqueue should
     * already guarantee the queue is empty by the time a frame's decode
     * finishes, but flush defensively in case a frame ever has any
     * still-pending macroblocks reaching this point - and this also
     * resets g_pending_n cleanly before this function's next call (the
     * CPU-only reference pass never enqueues anything, but no harm in
     * always leaving state clean either way). */
    flush_pending();
    ff_x1900_set_mb_hook(NULL, NULL);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return result;
}

/* Item 10: a real continuous multi-frame run, as opposed to every prior
 * milestone's independent TARGET_FRAME=N invocations (each of which
 * re-decodes 0..N from scratch inside decode_to_frame and only ever
 * times/compares the LAST frame of that run - N cumulative decodes,
 * never a genuine per-frame steady-state number). This walks the same
 * decode loop once, capturing every one of the first max_frames DISPLAY-
 * order output frames (avcodec_receive_frame already reorders B-frames
 * into display order, matching what target_frame always selected) plus
 * the wall-clock gap between successive frame emissions - a real
 * per-frame timing, not a cumulative one. Returns the number of frames
 * actually captured (may be less than max_frames if the clip is shorter).
 */
static int decode_multi(Mp4Movie *mov, unsigned char *avcc, int alen, int hook_live,
                         int max_frames, AVFrame **out_frames, double *out_times) {
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(alen + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)alen); ctx->extradata_size = alen;
    ctx->thread_count = 1;
    ctx->thread_type = 0;
    if (getenv("NO_DEBLOCK")) ctx->skip_loop_filter = AVDISCARD_ALL;
    avcodec_open2(ctx, codec, NULL);

    g_live = hook_live;
    ff_x1900_set_postpone_wanted(hook_live); /* item 9: keep the ref pass on the normal, unpostponed path - see x1900_hook.h's comment */
    g_took_over = 0; g_declined = 0; g_skip_took_over = 0; g_skip_declined = 0;
    g_p16_took_over = 0; g_p16_declined = 0;
    g_p16x8_took_over = 0; g_p16x8_declined = 0; g_p16x8_track_n = 0;
    g_p8x8_took_over = 0; g_p8x8_declined = 0; g_p8x8_track_n = 0; g_x1900_debug_frameno = -1;
    g_pending_n = 0; g_mc_pending_n = 0; g_cur_mb_mc_n = 0; g_lumadc_pending_n = 0;
    g_prof_lumadc_ms = g_prof_idct_ms = g_prof_mc_ms = 0;
    g_prof_lumadc_cpu_ms = g_prof_idct_cpu_ms = g_prof_mc_cpu_ms = 0;
    g_prof_sp_pack_ms = g_prof_sp_pack_cpu_ms = g_prof_sp_upload_ms = g_prof_sp_upload_cpu_ms = 0;
    g_prof_sp_draw_ms = g_prof_sp_draw_cpu_ms = g_prof_sp_read_ms = g_prof_sp_read_cpu_ms = 0; g_prof_sp_chunks = 0;
    g_prof_diag_ms = g_prof_diag_cpu_ms = 0; g_prof_diag_n = g_prof_diag_chunks = 0;
    g_prof_ib_pack_ms = g_prof_ib_pack_cpu_ms = g_prof_ib_upload_ms = g_prof_ib_upload_cpu_ms = 0;
    g_prof_ib_draw_ms = g_prof_ib_draw_cpu_ms = g_prof_ib_read_ms = g_prof_ib_read_cpu_ms = 0;
    g_prof_reftex_hit_ms = g_prof_reftex_hit_cpu_ms = g_prof_reftex_miss_ms = g_prof_reftex_miss_cpu_ms = 0;
    g_prof_reftex_hit_n = g_prof_reftex_miss_n = 0;
    g_prof_lumadc_n = g_prof_idct_n = g_prof_mc_n = g_prof_flush_n = 0;
    reftex_cache_reset();
    ff_x1900_set_mb_hook(live_hook, NULL);

    AVPacket *pkt = av_packet_alloc(); AVFrame *frame = av_frame_alloc();
    int frame_idx = 0;
    struct timeval tprev, tnow;
    gettimeofday(&tprev, NULL);
    int dbg_send_timing = getenv("DEBUG_SEND_TIMING") != NULL;
    for (uint32_t i = 0; i < mov->sample_count && frame_idx < max_frames; i++) {
        Mp4Sample *s = &mov->samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov->file_data + s->offset, s->size);
        struct timeval s0, s1;
        if (dbg_send_timing) gettimeofday(&s0, NULL);
        avcodec_send_packet(ctx, pkt); av_packet_unref(pkt);
        if (dbg_send_timing) {
            gettimeofday(&s1, NULL);
            double sms = (s1.tv_sec - s0.tv_sec) * 1000.0 + (s1.tv_usec - s0.tv_usec) / 1000.0;
            fprintf(stderr, "SEND_TIMING: sample=%u decode_frameno=%d send_ms=%.1f\n",
                    i, g_x1900_debug_frameno, sms);
        }
        while (frame_idx < max_frames && avcodec_receive_frame(ctx, frame) == 0) {
            gettimeofday(&tnow, NULL);
            out_times[frame_idx] = (tnow.tv_sec - tprev.tv_sec) * 1000.0 +
                                    (tnow.tv_usec - tprev.tv_usec) / 1000.0;
            tprev = tnow;
            out_frames[frame_idx] = av_frame_alloc();
            av_frame_ref(out_frames[frame_idx], frame);
            av_frame_unref(frame);
            frame_idx++;
        }
    }
    flush_pending();
    ff_x1900_set_mb_hook(NULL, NULL);
    av_packet_free(&pkt);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return frame_idx;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]); return 1; }
    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) { fprintf(stderr, "demux failed\n"); return 1; }
    int alen = 0; unsigned char *avcc = mp4_build_avcc(&mov, &alen);
    printf("Movie has %u samples (encoded frames)\n", mov.sample_count);

    GLint attribs[] = {AGL_RGBA, AGL_RED_SIZE, 8, AGL_GREEN_SIZE, 8,
                        AGL_BLUE_SIZE, 8, AGL_ALPHA_SIZE, 8,
                        AGL_DEPTH_SIZE, 24, AGL_NONE};
    AGLPixelFormat pf = aglChoosePixelFormat(NULL, 0, attribs);
    g_glctx = aglCreateContext(pf, NULL); aglDestroyPixelFormat(pf);
    /* Sized for cross-macroblock batching's widest possible viewport:
     * PENDING_MAX (40) macroblocks * 24 blocks * 4 texels = 3840 wide.
     * Quirk #15: this driver ties FBO/rendering bounds to the Pbuffer's
     * own drawable size, so undersizing this doesn't error - it
     * silently corrupts/truncates any draw past the Pbuffer's own
     * bounds. This was actually hit (not just theoretical): an earlier
     * 128-wide Pbuffer left over from the per-macroblock-only batching
     * milestone caused exactly this once real multi-macroblock batches
     * (width > 128) started happening - a 61% pixel mismatch that had
     * nothing to do with the batching logic itself. IDCT_BATCH_MAX's
     * own derivation already stays under GL_MAX_TEXTURE_SIZE=4096 (M4
     * probe), matched here. Height bumped 16->32 for item 4/phase 4d's
     * diagonal-family MC batch, whose stage-1 FBO render needs 21 rows
     * (-2..h+2 for h=16) - same quirk #15 hazard, this time on height
     * instead of width. */
    AGLPbuffer pbuf; aglCreatePBuffer(4096, 32, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 0, &pbuf);
    aglSetPBuffer(g_glctx, pbuf, 0, 0, 0); aglSetCurrentContext(g_glctx);

    /* Item 10: real continuous multi-frame run. Separate code path from
     * everything below (which re-decodes 0..TARGET_FRAME from scratch per
     * invocation and only ever looks at the last frame) - this decodes a
     * real run of MULTI_FRAME frames once, live and reference, and reports
     * genuine per-frame timing plus a per-frame mismatch breakdown. */
    if (getenv("MULTI_FRAME")) {
        int n = atoi(getenv("MULTI_FRAME"));
        if (n < 1) n = 10;
        if ((uint32_t)n > mov.sample_count) n = (int)mov.sample_count;
        AVFrame **live_frames = calloc((size_t)n, sizeof(AVFrame *));
        AVFrame **ref_frames = calloc((size_t)n, sizeof(AVFrame *));
        double *live_times = calloc((size_t)n, sizeof(double));
        double *ref_times = calloc((size_t)n, sizeof(double));

        printf("Multi-frame continuous decode: hook LIVE, %d frames...\n", n);
        int n_live = decode_multi(&mov, avcc, alen, 1, n, live_frames, live_times);
        int took_over = g_took_over, skip_took_over = g_skip_took_over, skip_declined = g_skip_declined;
        int p16_took_over = g_p16_took_over, p16_declined = g_p16_declined;
        int p16x8_took_over = g_p16x8_took_over, p16x8_declined = g_p16x8_declined;
        int p8x8_took_over = g_p8x8_took_over, p8x8_declined = g_p8x8_declined;
        int declined = g_declined;
        printf("  live totals: %d intra, %d skip (%d declined), %d P_16x16 (%d declined), "
               "%d P_16x8/P_8x16 (%d declined), %d P_8x8 (%d declined), %d declined to CPU\n",
               took_over, skip_took_over, skip_declined, p16_took_over, p16_declined,
               p16x8_took_over, p16x8_declined, p8x8_took_over, p8x8_declined, declined);
        if (getenv("DEBUG_GPU_PROFILE")) {
            printf("  GPU dispatch profile: %d flush_pending() calls; "
                   "lumadc: %d calls, %.1fms total (%.2fms/call); "
                   "idct_batch: %d calls, %.1fms total (%.2fms/call); "
                   "resolve_mc_pending: %d calls, %.1fms total (%.2fms/call)\n",
                   g_prof_flush_n,
                   g_prof_lumadc_n, g_prof_lumadc_ms, g_prof_lumadc_n ? g_prof_lumadc_ms / g_prof_lumadc_n : 0.0,
                   g_prof_idct_n, g_prof_idct_ms, g_prof_idct_n ? g_prof_idct_ms / g_prof_idct_n : 0.0,
                   g_prof_mc_n, g_prof_mc_ms, g_prof_mc_n ? g_prof_mc_ms / g_prof_mc_n : 0.0);
            double tot_wall = g_prof_lumadc_ms + g_prof_idct_ms + g_prof_mc_ms;
            double tot_cpu = g_prof_lumadc_cpu_ms + g_prof_idct_cpu_ms + g_prof_mc_cpu_ms;
            printf("  CPU-time breakdown (getrusage, item 9 investigation): "
                   "lumadc cpu=%.1fms (%.0f%% of its wall); "
                   "idct_batch cpu=%.1fms (%.0f%% of its wall); "
                   "resolve_mc_pending cpu=%.1fms (%.0f%% of its wall); "
                   "TOTAL cpu=%.1fms / wall=%.1fms (%.0f%%)\n",
                   g_prof_lumadc_cpu_ms, g_prof_lumadc_ms ? 100.0*g_prof_lumadc_cpu_ms/g_prof_lumadc_ms : 0.0,
                   g_prof_idct_cpu_ms, g_prof_idct_ms ? 100.0*g_prof_idct_cpu_ms/g_prof_idct_ms : 0.0,
                   g_prof_mc_cpu_ms, g_prof_mc_ms ? 100.0*g_prof_mc_cpu_ms/g_prof_mc_ms : 0.0,
                   tot_cpu, tot_wall, tot_wall ? 100.0*tot_cpu/tot_wall : 0.0);
            printf("  MC singlepass phase breakdown: pack wall=%.1fms cpu=%.1fms (%.0f%%); "
                   "upload wall=%.1fms cpu=%.1fms (%.0f%%); "
                   "draw+finish wall=%.1fms cpu=%.1fms (%.0f%%); "
                   "readback+unpack wall=%.1fms cpu=%.1fms (%.0f%%); %d chunks\n",
                   g_prof_sp_pack_ms, g_prof_sp_pack_cpu_ms, g_prof_sp_pack_ms ? 100.0*g_prof_sp_pack_cpu_ms/g_prof_sp_pack_ms : 0.0,
                   g_prof_sp_upload_ms, g_prof_sp_upload_cpu_ms, g_prof_sp_upload_ms ? 100.0*g_prof_sp_upload_cpu_ms/g_prof_sp_upload_ms : 0.0,
                   g_prof_sp_draw_ms, g_prof_sp_draw_cpu_ms, g_prof_sp_draw_ms ? 100.0*g_prof_sp_draw_cpu_ms/g_prof_sp_draw_ms : 0.0,
                   g_prof_sp_read_ms, g_prof_sp_read_cpu_ms, g_prof_sp_read_ms ? 100.0*g_prof_sp_read_cpu_ms/g_prof_sp_read_ms : 0.0,
                   g_prof_sp_chunks);
            printf("  MC diag-family total: %d resolve_mc_pending calls used it, %d chunks (MC_BATCH_MAXW cap), "
                   "wall=%.1fms cpu=%.1fms (%.0f%%), %.2fms/chunk\n",
                   g_prof_diag_n, g_prof_diag_chunks, g_prof_diag_ms, g_prof_diag_cpu_ms,
                   g_prof_diag_ms ? 100.0*g_prof_diag_cpu_ms/g_prof_diag_ms : 0.0,
                   g_prof_diag_chunks ? g_prof_diag_ms/g_prof_diag_chunks : 0.0);
            printf("  reftex_lookup_or_upload: %d hits (wall=%.1fms cpu=%.1fms, %.3fms/hit); "
                   "%d misses (wall=%.1fms cpu=%.1fms, %.2fms/miss)\n",
                   g_prof_reftex_hit_n, g_prof_reftex_hit_ms, g_prof_reftex_hit_cpu_ms,
                   g_prof_reftex_hit_n ? g_prof_reftex_hit_ms/g_prof_reftex_hit_n : 0.0,
                   g_prof_reftex_miss_n, g_prof_reftex_miss_ms, g_prof_reftex_miss_cpu_ms,
                   g_prof_reftex_miss_n ? g_prof_reftex_miss_ms/g_prof_reftex_miss_n : 0.0);
            printf("  IDCT batch phase breakdown: pack wall=%.1fms cpu=%.1fms (%.0f%%); "
                   "upload wall=%.1fms cpu=%.1fms (%.0f%%); "
                   "draw+finish wall=%.1fms cpu=%.1fms (%.0f%%); "
                   "readback+unpack wall=%.1fms cpu=%.1fms (%.0f%%)\n",
                   g_prof_ib_pack_ms, g_prof_ib_pack_cpu_ms, g_prof_ib_pack_ms ? 100.0*g_prof_ib_pack_cpu_ms/g_prof_ib_pack_ms : 0.0,
                   g_prof_ib_upload_ms, g_prof_ib_upload_cpu_ms, g_prof_ib_upload_ms ? 100.0*g_prof_ib_upload_cpu_ms/g_prof_ib_upload_ms : 0.0,
                   g_prof_ib_draw_ms, g_prof_ib_draw_cpu_ms, g_prof_ib_draw_ms ? 100.0*g_prof_ib_draw_cpu_ms/g_prof_ib_draw_ms : 0.0,
                   g_prof_ib_read_ms, g_prof_ib_read_cpu_ms, g_prof_ib_read_ms ? 100.0*g_prof_ib_read_cpu_ms/g_prof_ib_read_ms : 0.0);
        }

        printf("Multi-frame continuous decode: CPU-only reference, %d frames...\n", n);
        int n_ref = decode_multi(&mov, avcc, alen, 0, n, ref_frames, ref_times);

        int nn = n_live < n_ref ? n_live : n_ref;
        if (nn < n_live || nn < n_ref)
            fprintf(stderr, "warning: live captured %d frames, ref captured %d - clip may be "
                             "shorter than requested or decode stalled; comparing %d\n",
                    n_live, n_ref, nn);

        double live_total = 0, ref_total = 0, live_warm = 0, ref_warm = 0;
        int any_significant = 0;
        printf("\nPer-frame (display order), live-GPU-integrated vs CPU-only reference:\n");
        for (int f = 0; f < nn; f++) {
            AVFrame *live = live_frames[f], *ref = ref_frames[f];
            int w = live->width, h = live->height;
            long mismatches_y = 0, total_y = 0, max_diff_y = 0;
            for (int r = 0; r < h; r++)
                for (int c = 0; c < w; c++) {
                    int a = live->data[0][r * live->linesize[0] + c];
                    int b = ref->data[0][r * ref->linesize[0] + c];
                    int d = abs(a - b);
                    total_y++;
                    if (d > max_diff_y) max_diff_y = d;
                    if (d > 2) mismatches_y++;
                }
            long mismatches_c = 0, total_c = 0, max_diff_c = 0;
            for (int plane = 1; plane <= 2; plane++)
                for (int r = 0; r < h / 2; r++)
                    for (int c = 0; c < w / 2; c++) {
                        int a = live->data[plane][r * live->linesize[plane] + c];
                        int b = ref->data[plane][r * ref->linesize[plane] + c];
                        int d = abs(a - b);
                        total_c++;
                        if (d > max_diff_c) max_diff_c = d;
                        if (d > 2) mismatches_c++;
                    }
            long total = total_y + total_c, mismatches = mismatches_y + mismatches_c;
            double pct = 100.0 * mismatches / total;
            printf("  frame %2d: live=%7.1fms ref=%6.1fms  luma %ld/%ld (max %ld) chroma %ld/%ld "
                   "(max %ld)  %.3f%% mismatch%s\n",
                   f, live_times[f], ref_times[f], mismatches_y, total_y, max_diff_y,
                   mismatches_c, total_c, max_diff_c, pct, pct >= 1.0 ? "  <-- SIGNIFICANT" : "");
            live_total += live_times[f]; ref_total += ref_times[f];
            if (f > 0) { live_warm += live_times[f]; ref_warm += ref_times[f]; }
            if (pct >= 1.0) any_significant = 1;
        }
        printf("\nTotals over %d frames: live=%.1fms (%.1fms/frame avg) ref=%.1fms (%.1fms/frame avg)\n",
               nn, live_total, live_total / nn, ref_total, ref_total / nn);
        if (nn > 1)
            printf("Steady-state (excludes frame 0, which includes GL/shader warm-up): "
                   "live=%.1fms/frame ref=%.1fms/frame -> GPU is %.2fx %s CPU\n",
                   live_warm / (nn - 1), ref_warm / (nn - 1),
                   live_warm > ref_warm ? live_warm / ref_warm : ref_warm / live_warm,
                   live_warm > ref_warm ? "slower than" : "faster than");

        /* Item 9: the literal "is this real-time" check the plan's new goal
         * asks for - a hard per-frame budget, not just "faster than before."
         * RT_BUDGET_MS defaults to 33.3ms (30fps) since this project's demux
         * doesn't parse mvhd/stts timescale (deliberately minimal box
         * parser) - override with the content's real fps if known. */
        {
            double budget = getenv("RT_BUDGET_MS") ? atof(getenv("RT_BUDGET_MS")) : 33.333;
            int rt_ok = 0;
            for (int f = 1; f < nn; f++) if (live_times[f] <= budget) rt_ok++;
            printf("\nReal-time check (budget=%.1fms/frame, assumed 30fps unless RT_BUDGET_MS set): "
                   "%d/%d frames (excl. frame 0) met budget (%.0f%%) - %s\n",
                   budget, rt_ok, nn - 1, nn > 1 ? 100.0 * rt_ok / (nn - 1) : 0.0,
                   (nn > 1 && rt_ok == nn - 1) ? "REAL-TIME ACHIEVED" : "NOT real-time yet");
        }

        aglSetCurrentContext(NULL);
        aglDestroyContext(g_glctx);
        return any_significant;
    }

    /* TARGET_FRAME=0 (default) exercises the all-intra IDR path from
     * last session; TARGET_FRAME=1 reaches the first real P-frame,
     * needed to exercise MB_TYPE_SKIP inter reconstruction at all. */
    int target_frame = getenv("TARGET_FRAME") ? atoi(getenv("TARGET_FRAME")) : 0;

    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    printf("Decoding with GPU hook LIVE (hook returns 1 for qualifying macroblocks)...\n");
    AVFrame *live = decode_to_frame(&mov, avcc, alen, 1, target_frame);
    gettimeofday(&t1, NULL);
    double live_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
    int took_over = g_took_over; /* decode_to_frame resets these counters on
                                   * every call, including the reference
                                   * pass below - capture them now. */
    int skip_took_over = g_skip_took_over, skip_declined = g_skip_declined;
    int p16_took_over = g_p16_took_over, p16_declined = g_p16_declined;
    int p16x8_took_over = g_p16x8_took_over, p16x8_declined = g_p16x8_declined;
    static int p16x8_track_x[MAX_TRACK], p16x8_track_y[MAX_TRACK];
    int p16x8_track_n = g_p16x8_track_n;
    memcpy(p16x8_track_x, g_p16x8_track_x, sizeof(int) * p16x8_track_n);
    memcpy(p16x8_track_y, g_p16x8_track_y, sizeof(int) * p16x8_track_n);
    int p8x8_took_over = g_p8x8_took_over, p8x8_declined = g_p8x8_declined;
    static int p8x8_track_x[MAX_TRACK], p8x8_track_y[MAX_TRACK];
    int p8x8_track_n = g_p8x8_track_n;
    memcpy(p8x8_track_x, g_p8x8_track_x, sizeof(int) * p8x8_track_n);
    memcpy(p8x8_track_y, g_p8x8_track_y, sizeof(int) * p8x8_track_n);
    printf("  live decode: %.1f ms, %d intra macroblocks GPU-reconstructed, "
           "%d skip macroblocks MC-reconstructed (%d skip declined), "
           "%d P_16x16 macroblocks MC+residual-reconstructed (%d P_16x16 declined), "
           "%d P_16x8/P_8x16 macroblocks MC+residual-reconstructed (%d declined), "
           "%d P_8x8 macroblocks MC+residual-reconstructed (%d declined), "
           "%d declined to CPU\n",
           live_ms, took_over, skip_took_over, skip_declined, p16_took_over, p16_declined,
           p16x8_took_over, p16x8_declined, p8x8_took_over, p8x8_declined, g_declined);

    printf("Decoding CPU-only reference (hook never installed)...\n");
    gettimeofday(&t0, NULL);
    AVFrame *ref = decode_to_frame(&mov, avcc, alen, 0, target_frame);
    gettimeofday(&t1, NULL);
    double ref_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 + (t1.tv_usec - t0.tv_usec) / 1000.0;
    printf("  reference decode: %.1f ms\n", ref_ms);

    if (!live || !ref) { fprintf(stderr, "decode failed\n"); return 1; }
    if (took_over == 0) { fprintf(stderr, "hook never took over any macroblock - nothing exercised\n"); return 1; }

    /* Item 9 frame-scale restructure investigation: dump the ref (hook-
     * uninstalled) frame's raw Y/Cb/Cr planes for direct external diffing -
     * isolates whether FFmpeg's own declined-macroblock path is affected
     * by X1900_FORCE_POSTPONE, independent of this project's own hook. */
    if (getenv("DEBUG_DUMP_REF")) {
        FILE *fp = fopen(getenv("DEBUG_DUMP_REF"), "wb");
        if (fp) {
            for (int r = 0; r < ref->height; r++) fwrite(ref->data[0] + r*ref->linesize[0], 1, ref->width, fp);
            for (int r = 0; r < ref->height/2; r++) fwrite(ref->data[1] + r*ref->linesize[1], 1, ref->width/2, fp);
            for (int r = 0; r < ref->height/2; r++) fwrite(ref->data[2] + r*ref->linesize[2], 1, ref->width/2, fp);
            fclose(fp);
        }
    }

    if (getenv("DEBUG_SKIP")) {
        /* Skip macroblocks have no same-frame dependency (motion comp
         * reads an already-finalized reference frame), so unlike intra
         * there's no propagation to untangle - a wrong one is a direct
         * bug in mc_luma16/mc_chroma8 or the MV/offset math feeding
         * them, not a downstream victim. Just find the worst one. */
        int worst = -1, worst_err = -1;
        for (int t = 0; t < g_skip_track_n; t++) {
            int mx = g_skip_track_x[t], my = g_skip_track_y[t], err = 0;
            for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) {
                int a = live->data[0][(my*16+r)*live->linesize[0] + (mx*16+c)];
                int b = ref->data[0][(my*16+r)*ref->linesize[0] + (mx*16+c)];
                err += abs(a - b);
            }
            if (err > worst_err) { worst_err = err; worst = t; }
        }
        if (worst >= 0) {
            int mx = g_skip_track_x[worst], my = g_skip_track_y[worst];
            fprintf(stderr, "worst skip MB(%d,%d) luma total_err=%d, live/ref:\n", mx, my, worst_err);
            for (int r = 0; r < 16; r++) {
                fprintf(stderr, "  row%2d: ", r);
                for (int c = 0; c < 16; c++) {
                    int a = live->data[0][(my*16+r)*live->linesize[0] + (mx*16+c)];
                    int b = ref->data[0][(my*16+r)*ref->linesize[0] + (mx*16+c)];
                    fprintf(stderr, "%3d/%3d ", a, b);
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "total skip MBs tracked: %d\n", g_skip_track_n);
    }

    if (getenv("DEBUG_P16")) {
        /* Same reasoning as DEBUG_SKIP above: P_16x16 macroblocks read only
         * the (already-finalized) reference frame, never same-frame
         * spatial context, so a wrong one is a direct bug in this MB's own
         * residual/cbp/nnz handling, not a downstream victim of an earlier
         * MB. Just find the worst one. */
        /* live/ref only hold pixels for target_frame's own decoded image -
         * decode_to_frame's live pass runs through every frame 0..
         * target_frame in one go, so g_p16_track_* can (and does, for any
         * target_frame > 0) accumulate entries from EARLIER frames too.
         * Comparing an earlier frame's tracked coordinate against THIS
         * frame's pixel buffer is meaningless (different real content at
         * the same mb_x/mb_y) - filter to target_frame's own entries only. */
        int worst = -1, worst_err = -1;
        for (int t = 0; t < g_p16_track_n; t++) {
            if (g_p16_track_frame[t] != g_x1900_debug_captured_frameno) continue;
            int mx = g_p16_track_x[t], my = g_p16_track_y[t], err = 0;
            for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) {
                int a = live->data[0][(my*16+r)*live->linesize[0] + (mx*16+c)];
                int b = ref->data[0][(my*16+r)*ref->linesize[0] + (mx*16+c)];
                err += abs(a - b);
            }
            if (err > worst_err) { worst_err = err; worst = t; }
        }
        if (worst >= 0) {
            int mx = g_p16_track_x[worst], my = g_p16_track_y[worst];
            fprintf(stderr, "worst P16 MB(%d,%d) luma total_err=%d, live/ref:\n", mx, my, worst_err);
            for (int r = 0; r < 16; r++) {
                fprintf(stderr, "  row%2d: ", r);
                for (int c = 0; c < 16; c++) {
                    int a = live->data[0][(my*16+r)*live->linesize[0] + (mx*16+c)];
                    int b = ref->data[0][(my*16+r)*ref->linesize[0] + (mx*16+c)];
                    fprintf(stderr, "%3d/%3d ", a, b);
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "total P16 MBs tracked: %d\n", g_p16_track_n);
    }

    if (getenv("DEBUG_CHROMA")) {
        int worst = -1, worst_err = -1;
        int first_bad = -1, first_bad_err = 0;
        for (int t = 0; t < g_track_n; t++) {
            int mx = g_track_x[t], my = g_track_y[t], err = 0;
            for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
                int a = live->data[1][(my*8+r)*live->linesize[1] + (mx*8+c)];
                int b = ref->data[1][(my*8+r)*ref->linesize[1] + (mx*8+c)];
                err += abs(a - b);
            }
            if (err > worst_err) { worst_err = err; worst = t; }
            if (first_bad < 0 && err > 200) { first_bad = t; first_bad_err = err; }
        }
        if (first_bad >= 0)
            fprintf(stderr, "FIRST bad MB in decode order: track#%d MB(%d,%d) mode=%d err=%d\n",
                    first_bad, g_track_x[first_bad], g_track_y[first_bad], g_track_mode[first_bad], first_bad_err);

        /* Find the true origin: the first tracked MB whose INPUT neighbor
         * context (read from `ref`, i.e. what it SHOULD have seen if
         * every earlier MB were correct) matches `ref`'s own left/top -
         * trivially true for every MB since ref IS the ground truth -
         * so instead compare `ref`'s neighbor context against what THIS
         * MB's own output looks like: if a MB's neighbor pixels in `ref`
         * are self-consistent (obviously true) but `live`'s pixels AT
         * THIS MB diverge from `ref` while `live`'s pixels at its
         * neighbor positions still MATCH `ref`, this MB is the true
         * origin - everything upstream of it was still correct. */
        for (int t = 0; t < g_track_n; t++) {
            int mx = g_track_x[t], my = g_track_y[t];
            int uls_l = live->linesize[1], uls_r = ref->linesize[1];
            uint8_t *lcb = live->data[1] + my*8*uls_l + mx*8;
            uint8_t *rcb = ref->data[1] + my*8*uls_r + mx*8;
            int neighbor_ok = 1;
            for (int i = 0; i < 8 && neighbor_ok; i++) {
                if (lcb[-1+i*uls_l] != rcb[-1+i*uls_r]) neighbor_ok = 0;
                if (lcb[i-uls_l] != rcb[i-uls_r]) neighbor_ok = 0;
            }
            if (lcb[-1-uls_l] != rcb[-1-uls_r]) neighbor_ok = 0;
            if (!neighbor_ok) continue;
            int own_err = 0;
            for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++)
                own_err += abs(lcb[r*uls_l+c] - rcb[r*uls_r+c]);
            if (own_err > 8) {
                fprintf(stderr, "TRUE ORIGIN: track#%d MB(%d,%d) mode=%d - correct neighbor "
                        "context in, own_err=%d out\n", t, mx, my, g_track_mode[t], own_err);
                break;
            }
        }
        if (getenv("DEBUG_CHROMA_MB")) {
            int tx = atoi(getenv("DEBUG_CHROMA_MB")), ty = atoi(getenv("DEBUG_CHROMA_MB2"));
            for (int t = 0; t < g_track_n; t++)
                if (g_track_x[t] == tx && g_track_y[t] == ty) {
                    worst = t;
                    worst_err = 0; /* recompute - the natural "worst" search's value is stale here */
                    int mx2 = g_track_x[t], my2 = g_track_y[t];
                    for (int r = 0; r < 8; r++) for (int c = 0; c < 8; c++) {
                        int a = live->data[1][(my2*8+r)*live->linesize[1] + (mx2*8+c)];
                        int b = ref->data[1][(my2*8+r)*ref->linesize[1] + (mx2*8+c)];
                        worst_err += abs(a - b);
                    }
                    break;
                }
        }
        if (worst >= 0) {
            int mx = g_track_x[worst], my = g_track_y[worst];
            int uls_ref = ref->linesize[1];
            uint8_t *refcb = ref->data[1] + my*8*uls_ref + mx*8;
            fprintf(stderr, "ref Cb neighbor context for MB(%d,%d): left=", mx, my);
            for (int i=0;i<8;i++) fprintf(stderr,"%d ", refcb[-1+i*uls_ref]);
            fprintf(stderr,"\n  top=");
            for (int i=0;i<8;i++) fprintf(stderr,"%d ", refcb[i-uls_ref]);
            fprintf(stderr,"\n  topleft=%d\n", refcb[-1-uls_ref]);
            fprintf(stderr, "worst chroma MB(%d,%d) mode=%d total_err=%d - Cb plane, live/ref:\n",
                    mx, my, g_track_mode[worst], worst_err);
            for (int r = 0; r < 8; r++) {
                fprintf(stderr, "  row%d: ", r);
                for (int c = 0; c < 8; c++) {
                    int a = live->data[1][(my*8+r)*live->linesize[1] + (mx*8+c)];
                    int b = ref->data[1][(my*8+r)*ref->linesize[1] + (mx*8+c)];
                    fprintf(stderr, "%3d/%3d ", a, b);
                }
                fprintf(stderr, "\n");
            }
        }
        fprintf(stderr, "total taken-over MBs tracked: %d\n", g_track_n);
    }

    int w = live->width, h = live->height;
    long mismatches_y = 0, total_y = 0, max_diff_y = 0;
    int max_diff_y_r = -1, max_diff_y_c = -1;
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) {
            int a = live->data[0][r * live->linesize[0] + c];
            int b = ref->data[0][r * ref->linesize[0] + c];
            int d = abs(a - b);
            total_y++;
            if (d > max_diff_y) { max_diff_y = d; max_diff_y_r = r; max_diff_y_c = c; }
            if (d > 2) mismatches_y++;
        }
    if (getenv("DEBUG_CHROMA") && max_diff_y_r >= 0)
        fprintf(stderr, "max luma diff %ld at pixel (row=%d,col=%d) -> MB(%d,%d)\n",
                max_diff_y, max_diff_y_r, max_diff_y_c, max_diff_y_c/16, max_diff_y_r/16);

    if (getenv("DEBUG_ROW_SCAN")) {
        int mbw = (w + 15) / 16, mbh = (h + 15) / 16;
        for (int my = 0; my < mbh; my++) {
            long rowmis = 0;
            for (int r = my*16; r < my*16+16 && r < h; r++)
                for (int c = 0; c < w; c++) {
                    int a = live->data[0][r*live->linesize[0]+c];
                    int b = ref->data[0][r*ref->linesize[0]+c];
                    if (abs(a-b) > 2) rowmis++;
                }
            fprintf(stderr, "MB row %2d: %ld luma mismatches\n", my, rowmis);
        }
        (void)mbw;
        /* Which specific macroblocks (by column) have real (>2) mismatches? */
        for (int my = 0; my < mbh; my++) {
            for (int mx = 0; mx < mbw; mx++) {
                long mbmis = 0; int mbmax = 0;
                for (int r = my*16; r < my*16+16 && r < h; r++)
                    for (int c = mx*16; c < mx*16+16 && c < w; c++) {
                        int a = live->data[0][r*live->linesize[0]+c];
                        int b = ref->data[0][r*ref->linesize[0]+c];
                        int d = abs(a-b);
                        if (d > 2) mbmis++;
                        if (d > mbmax) mbmax = d;
                    }
                if (mbmis > 0)
                    fprintf(stderr, "  MB(%d,%d): %ld px mismatch, max diff %d\n", mx, my, mbmis, mbmax);
            }
        }
    }

    /* Item 9 frame-scale restructure investigation: direct, frame-agnostic
     * dump of a macroblock's own 16x16 luma block (live vs ref), bypassing
     * the tracking-array tools above (which accumulate across every
     * internally-decoded frame, not just the captured/target one - a
     * real, pre-existing limitation, not something this investigation
     * introduced, but one that made those tools unreliable for this
     * specific check). DEBUG_EDGE_DUMP=mbx,mby. */
    if (getenv("DEBUG_EDGE_DUMP")) {
        int emx, emy;
        sscanf(getenv("DEBUG_EDGE_DUMP"), "%d,%d", &emx, &emy);
        fprintf(stderr, "EDGE DUMP MB(%d,%d) luma live/ref:\n", emx, emy);
        for (int r = 0; r < 16; r++) {
            fprintf(stderr, "  row%2d: ", r);
            for (int c = 0; c < 16; c++) {
                int rr = emy*16+r, cc = emx*16+c;
                int a = live->data[0][rr*live->linesize[0]+cc];
                int b = ref->data[0][rr*ref->linesize[0]+cc];
                fprintf(stderr, "%3d/%3d ", a, b);
            }
            fprintf(stderr, "\n");
        }
    }

    long mismatches_c = 0, total_c = 0, max_diff_c = 0;
    for (int plane = 1; plane <= 2; plane++)
        for (int r = 0; r < h / 2; r++)
            for (int c = 0; c < w / 2; c++) {
                int a = live->data[plane][r * live->linesize[plane] + c];
                int b = ref->data[plane][r * ref->linesize[plane] + c];
                int d = abs(a - b);
                total_c++;
                if (d > max_diff_c) max_diff_c = d;
                if (d > 2) mismatches_c++;
            }

    if (getenv("DEBUG_P16X8_SCAN")) {
        for (int t = 0; t < p16x8_track_n; t++) {
            int mx = p16x8_track_x[t], my = p16x8_track_y[t];
            int err = 0, worstpix = 0;
            for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) {
                int a = live->data[0][(my*16+r)*live->linesize[0] + (mx*16+c)];
                int b = ref->data[0][(my*16+r)*ref->linesize[0] + (mx*16+c)];
                int d = abs(a-b);
                err += d;
                if (d > worstpix) worstpix = d;
            }
            if (err > 0)
                fprintf(stderr, "P16X8 track MB(%d,%d): total_err=%d worst_pixel_diff=%d\n", mx, my, err, worstpix);
        }
        for (int t = 0; t < p8x8_track_n; t++) {
            int mx = p8x8_track_x[t], my = p8x8_track_y[t];
            int err = 0, worstpix = 0;
            for (int r = 0; r < 16; r++) for (int c = 0; c < 16; c++) {
                int a = live->data[0][(my*16+r)*live->linesize[0] + (mx*16+c)];
                int b = ref->data[0][(my*16+r)*ref->linesize[0] + (mx*16+c)];
                int d = abs(a-b);
                err += d;
                if (d > worstpix) worstpix = d;
            }
            if (err > 0)
                fprintf(stderr, "P8X8 track MB(%d,%d): total_err=%d worst_pixel_diff=%d\n", mx, my, err, worstpix);
        }
    }

    printf("\nFull-frame comparison, live-GPU-integrated decode vs CPU-only reference:\n");
    printf("  luma:   %ld/%ld pixels differ by >2 (max diff %ld)\n", mismatches_y, total_y, max_diff_y);
    printf("  chroma: %ld/%ld pixels differ by >2 (max diff %ld)\n", mismatches_c, total_c, max_diff_c);

    long total = total_y + total_c, mismatches = mismatches_y + mismatches_c;
    double pct = 100.0 * mismatches / total;
    printf("\n%s (%.3f%% of pixels differ)\n",
           pct < 1.0 ? "RESULT: live GPU-integrated decode matches CPU-only reference"
                      : "RESULT: SIGNIFICANT MISMATCH",
           pct);

    aglSetCurrentContext(NULL);
    aglDestroyContext(g_glctx);
    return pct >= 1.0;
}
