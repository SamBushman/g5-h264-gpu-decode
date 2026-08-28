/*
 * x1900_hook: the real interception point for GPU-assisted H.264
 * reconstruction on the G5/X1900 project (see the project's plan doc).
 *
 * Course correction from the original plan: a standard AVHWAccel's
 * decode_slice receives the RAW (still-undecoded) slice bitstream bytes,
 * because every real hwaccel (VDPAU/VAAPI/DXVA/...) hands entropy decoding
 * itself to a hardware ASIC - confirmed by reading vdpau_h264.c in this
 * source tree. The X1900 has no such ASIC, so that path is the wrong tool:
 * we want CPU-entropy-decoded, pre-reconstruction per-MB data instead,
 * which is what ff_h264_hl_decode_mb() (h264_mb.c) already has in hand
 * right before it calls the normal CPU IDCT/motion-comp/intra-pred path.
 *
 * This header declares a small, plain-C hook installed at exactly that
 * point. Deliberately uses no H264-internal types (H264Context,
 * H264SliceContext, etc.) so an external application can include this
 * header without dragging in FFmpeg-private internals - h264_mb.c (which
 * DOES have those types in scope already) is responsible for marshalling
 * into this plain struct at the call site.
 *
 * NOTE: switched from positional function-pointer arguments to a struct
 * (X1900MbInfo) once the field count grew past ~8 - positional args were
 * getting error-prone to extend/keep in sync across x1900_hook.c,
 * h264_mb.c's call site, and every test program. New fields should be
 * added to the END of the struct (never reordered/removed) so existing
 * callers built against an older layout still read valid data for the
 * fields they know about, even before they're rebuilt.
 */

#ifndef AVCODEC_X1900_HOOK_H
#define AVCODEC_X1900_HOOK_H

#include <stdint.h>

typedef struct X1900MbInfo {
    int mb_x, mb_y;         /* macroblock position (not pixel position) */
    int mb_type;             /* FFmpeg's internal mb_type bitfield - see
                               * h264dec.h/mpegutils.h's MB_TYPE_* flags,
                               * e.g. MB_TYPE_INTRA16x16 = (1<<1). This
                               * header deliberately has no H264 macros. */
    int qscale;               /* this MB's QP */
    const int16_t *coeffs;    /* sl->mb: dequantized coefficients,
                               * 16*48 int16_t (luma + 2x chroma blocks,
                               * scan order) */
    const uint8_t *nnz;       /* sl->non_zero_count_cache: 15*8 uint8_t,
                               * per-block nonzero-coefficient counts
                               * (64 = "not available") */
    const int16_t *mv_l0;     /* sl->mv_cache[0][scan8[0]]: 5*8 entries of
                               * (x,y) int16_t motion vectors, list 0
                               * (list 1 not exposed yet - v1 scope is
                               * single-reference P-slices) */
    const int8_t *ref_l0;     /* sl->ref_cache[0][scan8[0]]: 5*8 int8_t
                               * reference indices */
    const int16_t *luma_dc;   /* sl->mb_luma_dc[0]: 16 int16_t, the
                               * pre-Hadamard-transform DC coefficients
                               * for I16x16 macroblocks (undefined for
                               * any other mb_type - check
                               * mb_type & MB_TYPE_INTRA16x16 yourself) */
    int luma_dc_qmul;         /* h->ps.pps->dequant4_coeff[0][qscale][0]:
                               * exact dequant scale FFmpeg itself uses
                               * for luma_dc - passed through rather than
                               * re-derived to avoid duplicating PPS-table
                               * logic */
    uint8_t *dest_y;           /* pointer directly into the LIVE frame
                               * buffer (h->cur_pic.f->data[0]) at this
                               * macroblock's top-left luma pixel - safe
                               * to READ neighbor context from (left
                               * column/top row of already-reconstructed
                               * neighboring macroblocks, standard raster-
                               * order availability) even though nothing
                               * has been written here yet for THIS mb.
                               * Writing through this pointer is now a
                               * proven path (gpu-live-decode-test): write
                               * the real reconstructed 16x16 block here
                               * and return 1 to take over. Only do this
                               * for a macroblock you are fully
                               * reconstructing correctly - anything you
                               * write is immediately load-bearing as
                               * neighbor context for every subsequent
                               * macroblock's intra prediction. */
    int linesize;              /* stride for dest_y, in bytes/pixels */
    int intra16x16_pred_mode;  /* sl->intra16x16_pred_mode: 0=DC, 1=Horizontal,
                               * 2=Vertical, 3=Plane (FFmpeg's own internal
                               * enum order - see h264pred.h's *_PRED8x8
                               * defines - NOT the raw H.264 bitstream
                               * value's order). Only meaningful when
                               * mb_type & MB_TYPE_INTRA16x16. */

    /* Added for real integration (hook actually taking over, not just
     * observing): chroma is reconstructed unconditionally for every
     * intra macroblock regardless of luma sub-type, so a hook that
     * returns 1 for an intra16x16 macroblock must handle chroma too -
     * returning 1 skips ff_h264_hl_decode_mb() entirely, luma AND
     * chroma both. */
    uint8_t *dest_cb, *dest_cr; /* live frame buffer, chroma planes -
                               * same read-before-write-your-own-mb,
                               * read-only-for-neighbors contract as
                               * dest_y. */
    int uvlinesize;             /* stride for dest_cb/dest_cr */
    int chroma_pred_mode;       /* sl->chroma_pred_mode: 0=DC, 1=Horizontal,
                               * 2=Vertical, 3=Plane - same DC_PRED8x8/
                               * HOR_PRED8x8/VERT_PRED8x8/PLANE_PRED8x8
                               * encoding as intra16x16_pred_mode, just a
                               * separate FFmpeg enum (h264pred.h). Valid
                               * whenever this macroblock is intra (always
                               * true when MB_TYPE_INTRA16x16 is set). */
    int chroma_dc_qmul[2];      /* [0]=Cb, [1]=Cr. Exact
                               * h->ps.pps->dequant4_coeff[IS_INTRA?1:4][chroma_qp[0]][0]
                               * (Cb) / [...2:5...][chroma_qp[1]][0] (Cr) -
                               * passed through rather than re-derived, same
                               * rationale as luma_dc_qmul. */
    int cbp;                    /* sl->cbp (coded block pattern). Real FFmpeg
                               * gates its ENTIRE chroma-residual block -
                               * both the chroma-DC dequant/transform AND
                               * idct_add8 - on `cbp & 0x30`, a level above
                               * the individual non_zero_count_cache checks
                               * (h264_mb_template.c). If that bit is clear,
                               * chroma has NO residual at all for this MB
                               * and the real code never even looks at
                               * non_zero_count_cache/sl->mb for chroma -
                               * checking nnz alone (as an earlier version
                               * of this hook did) isn't sufficient, since
                               * nnz can appear set from stale/leftover
                               * state that real reconstruction never reads
                               * because cbp already said "skip". */

    /* Real integration finding: reading TOP-neighbor context directly
     * from the live frame buffer (dest_y[-linesize+...] etc) is WRONG
     * whenever mb_y>0 and this slice has deblocking enabled. FFmpeg's
     * own per-row decode loop calls loop_filter() once a whole row
     * finishes - BEFORE the next row's macroblocks reconstruct - so by
     * the time this hook fires for row mb_y, row mb_y-1's bottom edge
     * may already be POST-deblock in the live buffer, even though intra
     * prediction is spec-required to use PRE-deblock neighbor samples.
     * FFmpeg's real hl_decode_mb() (h264_mb.c's xchg_mb_border(),
     * called right before intra prediction, gated on
     * sl->deblocking_filter) works around exactly this by swapping the
     * live buffer's top-border row with a dedicated per-column cache
     * (sl->top_borders[]) that preserves the true pre-deblock values -
     * our hook fires before xchg_mb_border ever runs, so it never sees
     * that swap. LEFT-neighbor context has no equivalent problem (the
     * macroblock to the left is in the same not-yet-filtered row), so
     * dest_y/dest_cb/dest_cr remain correct and sufficient for that.
     *
     * top_border_here/top_border_left are raw sl->top_borders[1][mb_x]
     * / [mb_x-1] pointers (top_idx=1 always holds for non-MBAFF/
     * progressive content, which covers this project's whole scope) -
     * each a 96-byte-per-column cache entry (H264SliceContext's
     * `top_borders[2][...][(16*3)*2]`). For 8-bit 4:2:0 content the
     * layout (from h264_mb.c's xchg_mb_border offsets) is:
     *   [0..15]  = this column's luma top-neighbor row (16 px)
     *   [16..23] = this column's Cb top-neighbor row (8 px)
     *   [24..31] = this column's Cr top-neighbor row (8 px)
     * topleft corners come from the LEFT column's own entry, its last
     * byte within each region: top_border_left[15] (luma),
     * top_border_left[23] (Cb), top_border_left[31] (Cr). NULL when
     * mb_x==0 (no left column exists). */
    const uint8_t *top_border_here;
    const uint8_t *top_border_left;

    int mb_width;                /* h->mb_width: macroblock columns per
                               * row. A hook that defers work across
                               * multiple macroblocks (e.g. to batch GPU
                               * dispatch by row) MUST flush everything
                               * for a row proactively, the moment
                               * mb_x == mb_width-1 is seen for that row
                               * (checked on every call, not just ones
                               * the hook takes over) - not reactively on
                               * the next row's first call. FFmpeg's own
                               * loop_filter()/backup_mb_border() for a
                               * completed row run as soon as that row's
                               * last macroblock is decoded, which
                               * happens *inside* FFmpeg's C code between
                               * this hook's last call for that row and
                               * its first call for the next one - by the
                               * time a reactive "the row changed" check
                               * could fire, it's already too late, and
                               * loop_filter() has already read whichever
                               * pixels were live at that moment (found
                               * the hard way: a deferred macroblock's
                               * un-flushed all-zero placeholder pixels
                               * got baked into sl->top_borders[] for the
                               * row below to read, corrupting every
                               * macroblock that used it as top context). */
    int mb_height;               /* h->mb_height: total macroblock rows in
                               * the frame. Lets a hook that defers work
                               * across multiple macroblocks (e.g. to
                               * batch GPU dispatch by row) detect "this
                               * is the last row" - there's no future
                               * row-change event to trigger a final
                               * flush before FFmpeg's own per-row
                               * loop_filter()/backup_mb_border() run on
                               * it, so deferred work touching the last
                               * row must be flushed before returning
                               * from THIS call, not on the next one. */

    /* Inter (P-slice) support: list-0, single-reference only, matching
     * this project's whole declared scope (no B-frames, no list-1). Raw
     * REFERENCE-frame plane pointers (frame origin - NOT offset to this
     * macroblock's own position, unlike dest_y/dest_cb/dest_cr). This
     * matches real FFmpeg's own mc_dir_part(): the motion vector and
     * this macroblock's mb_x/mb_y (folded together, both in
     * quarter-luma-pel units: `mv + mb_x*64` etc, see mc_part_std's
     * `x_offset += 8*sl->mb_x` then mc_dir_part's `mx = mv + x_offset*8`)
     * are what locate the source position within the reference frame -
     * there is no per-macroblock pre-offset on the reference pointer
     * itself, since motion can (and does) point anywhere in the frame,
     * not just near this macroblock's own position. Valid (non-NULL)
     * only when IS_INTER(mb_type) and a real list-0 reference exists;
     * NULL for intra macroblocks or if ref_cache lookup failed. */
    const uint8_t *ref_y, *ref_cb, *ref_cr;
    int ref_linesize, ref_uvlinesize;

    /* sl->slice_type_nos - this macroblock's ENCLOSING SLICE's type,
     * collapsed to P/B/I (SP/SI folded into P/I - the "_nos" = "no S"
     * FFmpeg convention), using AV_PICTURE_TYPE_* from libavutil/avutil.h
     * (I=1, P=2, B=3). Real finding: MB_TYPE_L1 alone is NOT a reliable
     * B-slice detector for every inter macroblock type - it's known to
     * correctly catch B-slice SKIP (bi-predictive skip sets it), but a
     * B-slice macroblock using B_Direct or B_L0_16x16 prediction can carry
     * MB_TYPE_16x16 with MB_TYPE_L1 genuinely UNSET (a real, valid H.264
     * partition type that uses only one direction), while still not being
     * the simple, single-reference P-slice content this project's list-0-
     * only scope (see MB_TYPE_L1's own comment) is actually built for.
     * Check this field directly (== AV_PICTURE_TYPE_B) wherever B-slice
     * content must be declined outright, rather than trusting mb_type
     * flags alone to imply "this is really a P-slice macroblock". */
    int slice_type_nos;

    /* Sub-8x8 partition support (P_16x8/P_8x16/P_8x8 - see the plan's
     * "Item 3 (sub-8x8 partitions): DESIGN PASS"). List-0, single-shared-
     * reference only, matching this project's existing inter scope.
     *
     * ADDITIVE, not a replacement for mv_l0/ref_l0 above: those stay
     * exactly as they are (still just the single scan8[0] entry) so the
     * existing P_Skip/P_16x16 callers keep working unmodified - repointing
     * them at these wider arrays instead would silently break both
     * (mv_l0[0]/[1] would stop meaning "the scan8[0] vector" with no
     * compiler error, since it's a raw pointer).
     *
     * mv_l0_cache/ref_l0_cache genuinely ARE the full underlying arrays
     * (sl->mv_cache[0]/sl->ref_cache[0], 40 entries each - see
     * h264dec.h's `mv_cache[2][5*8][2]`/`ref_cache[2][5*8]`), safe to
     * index at any real scan8[n] for n=0..15 (the standard luma z-order
     * block numbering already used elsewhere in this project for IDCT/
     * luma-DC indexing - e.g. gpu_live_decode_test.c's own
     * `luma_ac_scan8[16]` table, reused here rather than duplicated: real
     * scan8[] values for n=0..15, verified directly against
     * h264_parse.h:40, are {12,13,20,21,14,15,22,23,28,29,36,37,30,31,
     * 38,39}). mv_l0_cache[2*scan8[n]+0/1] = (x,y); ref_l0_cache[scan8[n]]
     * = ref index, matching real FFmpeg's own mc_dir_part/mc_part_std
     * indexing (h264_mb.c) exactly. */
    const int16_t *mv_l0_cache;
    const int8_t *ref_l0_cache;

    /* sl->sub_mb_type[4], one per 8x8 quadrant (index order matches the
     * quadrant base block indices n=0,4,8,12), only meaningful when
     * mb_type & MB_TYPE_8x8. Real gotcha, verified from h264dec.h: the
     * IS_SUB_8X8/IS_SUB_8X4/IS_SUB_4X8/IS_SUB_4X4 macros REUSE
     * MB_TYPE_16x16/MB_TYPE_16x8/MB_TYPE_8x16/MB_TYPE_8x8's own bit
     * values, just applied to a sub_mb_type entry instead of mb_type -
     * "note reused" in FFmpeg's own source comment (h264dec.h:96-99). */
    int sub_mb_type[4];
} X1900MbInfo;

/*
 * Called once per macroblock, after entropy decode, before CPU
 * reconstruction would normally run.
 *
 * Return 1 if this macroblock was fully handled (skip FFmpeg's own CPU
 * reconstruction for it) or 0 to let FFmpeg reconstruct it normally (the
 * fallback path for anything not yet offloaded - CABAC, B-slices, etc.,
 * matching the plan's "decline and let software handle it" design).
 */
typedef int (*X1900MbHookFn)(const X1900MbInfo *info, void *userdata);

void ff_x1900_set_mb_hook(X1900MbHookFn fn, void *userdata);

/* Called from h264_mb.c - not part of the public API applications call
 * directly, but declared here since h264_mb.c needs it and this is the
 * one header both it and this file's own .c share. */
int ff_x1900_call_mb_hook(const X1900MbInfo *info);

/* Item 9 frame-scale restructure (2026-08-28): called from
 * h264_slice.c's ff_h264_execute_decode_slices to decide whether to
 * postpone this decode's deblocking (h->postpone_filter) - true only when
 * the test harness has explicitly opted in via ff_x1900_set_postpone_wanted
 * below (NOT simply "is a hook installed" - a real bug during this
 * project's own verification found the harness installs the same hook
 * object for both its "live" and CPU-only "ref" passes, so that check
 * alone would silently postpone deblocking for the ref pass too, breaking
 * its job as a stable, untouched baseline). See the plan's "Item 9
 * frame-scale restructure" write-up for the full design: when postponed,
 * intra-prediction top-context reads (both FFmpeg's own, via
 * xchg_mb_border, and this project's own hook, via a matching change)
 * read directly from the live (never-yet-deblocked) buffer instead of
 * the sl->top_borders[] cache, which is never populated (backup_mb_border
 * lives inside loop_filter, itself skipped while postponed) - safe
 * because nothing gets deblocked prematurely to corrupt what intra
 * prediction needs. Real FFmpeg-level finding, verified empirically
 * (X1900_FORCE_POSTPONE + DEBUG_DUMP_REF diffing, hook fully uninstalled
 * in both cases compared): FFmpeg's own postpone_filter mechanism
 * (designed for nb_slice_ctx>1, multi-threaded slice decode) produces
 * byte-identical output when forced on for the ordinary single-context
 * case too - not a documented/intended use, but verified correct on real
 * content before relying on it. */
int ff_x1900_hook_installed(void);

/* Called by the test harness (not h264_mb.c/h264_slice.c) whenever it
 * changes whether the installed hook is actually live (its own g_live
 * flag) - see ff_x1900_hook_installed's comment above for why this is
 * a separate signal from "is a hook object installed at all". */
void ff_x1900_set_postpone_wanted(int enable);

/*
 * Milestone 8 (deblocking): called right before FFmpeg would invoke its
 * own CPU h264_v_loop_filter_luma on one real macroblock edge, with the
 * real, already-computed alpha/beta/tc0 (from FFmpeg's own QP+offset
 * lookup tables - not recomputed by us), and a pointer directly into the
 * live frame buffer at the edge position (still unfiltered at hook time -
 * this fires before the real filter mutates it). Same non-invasive,
 * observe-only pattern as the mb hook: always return 0 (let the real
 * filter run) for now; the pixel pointer stays valid/readable-again after
 * decode as part of the final output frame, so a test can also read the
 * REAL post-filter result from the finished AVFrame at the same offset,
 * without this hook needing an "after" callback at all.
 */
typedef int (*X1900DeblockHookFn)(uint8_t *pix, int stride, int alpha, int beta,
                                   const int8_t *tc0, void *userdata);

void ff_x1900_set_deblock_hook(X1900DeblockHookFn fn, void *userdata);

int ff_x1900_call_deblock_hook(uint8_t *pix, int stride, int alpha, int beta,
                                const int8_t *tc0);

#endif
