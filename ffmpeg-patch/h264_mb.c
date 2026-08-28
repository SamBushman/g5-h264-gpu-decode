/*
 * H.26L/H.264/AVC/JVT/14496-10/... decoder
 * Copyright (c) 2003 Michael Niedermayer <michaelni@gmx.at>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

/**
 * @file
 * H.264 / AVC / MPEG-4 part10 macroblock decoding
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include "libavutil/common.h"
#include "libavutil/intreadwrite.h"
#include "avcodec.h"
#include "h264dec.h"
#include "h264_ps.h"
#include "x1900_hook.h"
#include "qpeldsp.h"
#include "rectangle.h"
#include "threadframe.h"

static inline int get_lowest_part_list_y(H264SliceContext *sl,
                                         int n, int height, int y_offset, int list)
{
    int raw_my             = sl->mv_cache[list][scan8[n]][1];
    int filter_height_down = (raw_my & 3) ? 3 : 0;
    int full_my            = (raw_my >> 2) + y_offset;
    int bottom             = full_my + filter_height_down + height;

    av_assert2(height >= 0);

    return FFMAX(0, bottom);
}

static inline void get_lowest_part_y(const H264Context *h, H264SliceContext *sl,
                                     int16_t refs[2][48], int n,
                                     int height, int y_offset, int list0,
                                     int list1, int *nrefs)
{
    int my;

    y_offset += 16 * (sl->mb_y >> MB_FIELD(sl));

    if (list0) {
        int ref_n = sl->ref_cache[0][scan8[n]];
        H264Ref *ref = &sl->ref_list[0][ref_n];

        // Error resilience puts the current picture in the ref list.
        // Don't try to wait on these as it will cause a deadlock.
        // Fields can wait on each other, though.
        if (ref->parent->tf.progress != h->cur_pic.tf.progress ||
            (ref->reference & 3) != h->picture_structure) {
            my = get_lowest_part_list_y(sl, n, height, y_offset, 0);
            if (refs[0][ref_n] < 0)
                nrefs[0] += 1;
            refs[0][ref_n] = FFMAX(refs[0][ref_n], my);
        }
    }

    if (list1) {
        int ref_n    = sl->ref_cache[1][scan8[n]];
        H264Ref *ref = &sl->ref_list[1][ref_n];

        if (ref->parent->tf.progress != h->cur_pic.tf.progress ||
            (ref->reference & 3) != h->picture_structure) {
            my = get_lowest_part_list_y(sl, n, height, y_offset, 1);
            if (refs[1][ref_n] < 0)
                nrefs[1] += 1;
            refs[1][ref_n] = FFMAX(refs[1][ref_n], my);
        }
    }
}

/**
 * Wait until all reference frames are available for MC operations.
 *
 * @param h the H.264 context
 */
static void await_references(const H264Context *h, H264SliceContext *sl)
{
    const int mb_xy   = sl->mb_xy;
    const int mb_type = h->cur_pic.mb_type[mb_xy];
    int16_t refs[2][48];
    int nrefs[2] = { 0 };
    int ref, list;

    memset(refs, -1, sizeof(refs));

    if (IS_16X16(mb_type)) {
        get_lowest_part_y(h, sl, refs, 0, 16, 0,
                          IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1), nrefs);
    } else if (IS_16X8(mb_type)) {
        get_lowest_part_y(h, sl, refs, 0, 8, 0,
                          IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1), nrefs);
        get_lowest_part_y(h, sl, refs, 8, 8, 8,
                          IS_DIR(mb_type, 1, 0), IS_DIR(mb_type, 1, 1), nrefs);
    } else if (IS_8X16(mb_type)) {
        get_lowest_part_y(h, sl, refs, 0, 16, 0,
                          IS_DIR(mb_type, 0, 0), IS_DIR(mb_type, 0, 1), nrefs);
        get_lowest_part_y(h, sl, refs, 4, 16, 0,
                          IS_DIR(mb_type, 1, 0), IS_DIR(mb_type, 1, 1), nrefs);
    } else {
        int i;

        av_assert2(IS_8X8(mb_type));

        for (i = 0; i < 4; i++) {
            const int sub_mb_type = sl->sub_mb_type[i];
            const int n           = 4 * i;
            int y_offset          = (i & 2) << 2;

            if (IS_SUB_8X8(sub_mb_type)) {
                get_lowest_part_y(h, sl, refs, n, 8, y_offset,
                                  IS_DIR(sub_mb_type, 0, 0),
                                  IS_DIR(sub_mb_type, 0, 1),
                                  nrefs);
            } else if (IS_SUB_8X4(sub_mb_type)) {
                get_lowest_part_y(h, sl, refs, n, 4, y_offset,
                                  IS_DIR(sub_mb_type, 0, 0),
                                  IS_DIR(sub_mb_type, 0, 1),
                                  nrefs);
                get_lowest_part_y(h, sl, refs, n + 2, 4, y_offset + 4,
                                  IS_DIR(sub_mb_type, 0, 0),
                                  IS_DIR(sub_mb_type, 0, 1),
                                  nrefs);
            } else if (IS_SUB_4X8(sub_mb_type)) {
                get_lowest_part_y(h, sl, refs, n, 8, y_offset,
                                  IS_DIR(sub_mb_type, 0, 0),
                                  IS_DIR(sub_mb_type, 0, 1),
                                  nrefs);
                get_lowest_part_y(h, sl, refs, n + 1, 8, y_offset,
                                  IS_DIR(sub_mb_type, 0, 0),
                                  IS_DIR(sub_mb_type, 0, 1),
                                  nrefs);
            } else {
                int j;
                av_assert2(IS_SUB_4X4(sub_mb_type));
                for (j = 0; j < 4; j++) {
                    int sub_y_offset = y_offset + 2 * (j & 2);
                    get_lowest_part_y(h, sl, refs, n + j, 4, sub_y_offset,
                                      IS_DIR(sub_mb_type, 0, 0),
                                      IS_DIR(sub_mb_type, 0, 1),
                                      nrefs);
                }
            }
        }
    }

    for (list = sl->list_count - 1; list >= 0; list--)
        for (ref = 0; ref < 48 && nrefs[list]; ref++) {
            int row = refs[list][ref];
            if (row >= 0) {
                H264Ref *ref_pic  = &sl->ref_list[list][ref];
                int ref_field         = ref_pic->reference - 1;
                int ref_field_picture = ref_pic->parent->field_picture;
                int pic_height        = 16 * h->mb_height >> ref_field_picture;

                row <<= MB_MBAFF(sl);
                nrefs[list]--;

                if (!FIELD_PICTURE(h) && ref_field_picture) { // frame referencing two fields
                    av_assert2((ref_pic->parent->reference & 3) == 3);
                    ff_thread_await_progress(&ref_pic->parent->tf,
                                             FFMIN((row >> 1) - !(row & 1),
                                                   pic_height - 1),
                                             1);
                    ff_thread_await_progress(&ref_pic->parent->tf,
                                             FFMIN((row >> 1), pic_height - 1),
                                             0);
                } else if (FIELD_PICTURE(h) && !ref_field_picture) { // field referencing one field of a frame
                    ff_thread_await_progress(&ref_pic->parent->tf,
                                             FFMIN(row * 2 + ref_field,
                                                   pic_height - 1),
                                             0);
                } else if (FIELD_PICTURE(h)) {
                    ff_thread_await_progress(&ref_pic->parent->tf,
                                             FFMIN(row, pic_height - 1),
                                             ref_field);
                } else {
                    ff_thread_await_progress(&ref_pic->parent->tf,
                                             FFMIN(row, pic_height - 1),
                                             0);
                }
            }
        }
}

static av_always_inline void mc_dir_part(const H264Context *h, H264SliceContext *sl,
                                         H264Ref *pic,
                                         int n, int square, int height,
                                         int delta, int list,
                                         uint8_t *dest_y, uint8_t *dest_cb,
                                         uint8_t *dest_cr,
                                         int src_x_offset, int src_y_offset,
                                         const qpel_mc_func *qpix_op,
                                         h264_chroma_mc_func chroma_op,
                                         int pixel_shift, int chroma_idc)
{
    const int mx      = sl->mv_cache[list][scan8[n]][0] + src_x_offset * 8;
    int my            = sl->mv_cache[list][scan8[n]][1] + src_y_offset * 8;
    const int luma_xy = (mx & 3) + ((my & 3) << 2);
    ptrdiff_t offset  = (mx >> 2) * (1 << pixel_shift) + (my >> 2) * sl->mb_linesize;
    uint8_t *src_y    = pic->data[0] + offset;
    uint8_t *src_cb, *src_cr;
    int extra_width  = 0;
    int extra_height = 0;
    int emu = 0;
    const int full_mx    = mx >> 2;
    const int full_my    = my >> 2;
    if (getenv("DEBUG_MC_DIR") && sl->mb_x == atoi(getenv("DEBUG_MC_DIR")) &&
        sl->mb_y == (getenv("DEBUG_MC_DIR2") ? atoi(getenv("DEBUG_MC_DIR2")) : -999) && list == 0)
        fprintf(stderr, "REAL mc_dir_part MB(%d,%d) n=%d list=%d: mv_cache=(%d,%d) "
                "src_offset=(%d,%d) mx=%d my=%d h_phase=%d v_phase=%d full_mx=%d full_my=%d\n",
                sl->mb_x, sl->mb_y, n, list, sl->mv_cache[list][scan8[n]][0], sl->mv_cache[list][scan8[n]][1],
                src_x_offset, src_y_offset, mx, my, mx & 3, my & 3, full_mx, full_my);
    const int pic_width  = 16 * h->mb_width;
    const int pic_height = 16 * h->mb_height >> MB_FIELD(sl);
    int ysh;

    if (mx & 7)
        extra_width -= 3;
    if (my & 7)
        extra_height -= 3;

    if (full_mx                <          0 - extra_width  ||
        full_my                <          0 - extra_height ||
        full_mx + 16 /*FIXME*/ > pic_width  + extra_width  ||
        full_my + 16 /*FIXME*/ > pic_height + extra_height) {
        h->vdsp.emulated_edge_mc(sl->edge_emu_buffer,
                                 src_y - (2 << pixel_shift) - 2 * sl->mb_linesize,
                                 sl->mb_linesize, sl->mb_linesize,
                                 16 + 5, 16 + 5 /*FIXME*/, full_mx - 2,
                                 full_my - 2, pic_width, pic_height);
        src_y = sl->edge_emu_buffer + (2 << pixel_shift) + 2 * sl->mb_linesize;
        emu   = 1;
    }

    qpix_op[luma_xy](dest_y, src_y, sl->mb_linesize); // FIXME try variable height perhaps?
    if (!square)
        qpix_op[luma_xy](dest_y + delta, src_y + delta, sl->mb_linesize);

    if (CONFIG_GRAY && h->flags & AV_CODEC_FLAG_GRAY)
        return;

    if (chroma_idc == 3 /* yuv444 */) {
        src_cb = pic->data[1] + offset;
        if (emu) {
            h->vdsp.emulated_edge_mc(sl->edge_emu_buffer,
                                     src_cb - (2 << pixel_shift) - 2 * sl->mb_linesize,
                                     sl->mb_linesize, sl->mb_linesize,
                                     16 + 5, 16 + 5 /*FIXME*/,
                                     full_mx - 2, full_my - 2,
                                     pic_width, pic_height);
            src_cb = sl->edge_emu_buffer + (2 << pixel_shift) + 2 * sl->mb_linesize;
        }
        qpix_op[luma_xy](dest_cb, src_cb, sl->mb_linesize); // FIXME try variable height perhaps?
        if (!square)
            qpix_op[luma_xy](dest_cb + delta, src_cb + delta, sl->mb_linesize);

        src_cr = pic->data[2] + offset;
        if (emu) {
            h->vdsp.emulated_edge_mc(sl->edge_emu_buffer,
                                     src_cr - (2 << pixel_shift) - 2 * sl->mb_linesize,
                                     sl->mb_linesize, sl->mb_linesize,
                                     16 + 5, 16 + 5 /*FIXME*/,
                                     full_mx - 2, full_my - 2,
                                     pic_width, pic_height);
            src_cr = sl->edge_emu_buffer + (2 << pixel_shift) + 2 * sl->mb_linesize;
        }
        qpix_op[luma_xy](dest_cr, src_cr, sl->mb_linesize); // FIXME try variable height perhaps?
        if (!square)
            qpix_op[luma_xy](dest_cr + delta, src_cr + delta, sl->mb_linesize);
        return;
    }

    ysh = 3 - (chroma_idc == 2 /* yuv422 */);
    if (chroma_idc == 1 /* yuv420 */ && MB_FIELD(sl)) {
        // chroma offset when predicting from a field of opposite parity
        my  += 2 * ((sl->mb_y & 1) - (pic->reference - 1));
        emu |= (my >> 3) < 0 || (my >> 3) + 8 >= (pic_height >> 1);
    }

    src_cb = pic->data[1] + ((mx >> 3) * (1 << pixel_shift)) +
             (my >> ysh) * sl->mb_uvlinesize;
    src_cr = pic->data[2] + ((mx >> 3) * (1 << pixel_shift)) +
             (my >> ysh) * sl->mb_uvlinesize;

    if (emu) {
        h->vdsp.emulated_edge_mc(sl->edge_emu_buffer, src_cb,
                                 sl->mb_uvlinesize, sl->mb_uvlinesize,
                                 9, 8 * chroma_idc + 1, (mx >> 3), (my >> ysh),
                                 pic_width >> 1, pic_height >> (chroma_idc == 1 /* yuv420 */));
        src_cb = sl->edge_emu_buffer;
    }
    chroma_op(dest_cb, src_cb, sl->mb_uvlinesize,
              height >> (chroma_idc == 1 /* yuv420 */),
              mx & 7, ((unsigned)my << (chroma_idc == 2 /* yuv422 */)) & 7);

    if (emu) {
        h->vdsp.emulated_edge_mc(sl->edge_emu_buffer, src_cr,
                                 sl->mb_uvlinesize, sl->mb_uvlinesize,
                                 9, 8 * chroma_idc + 1, (mx >> 3), (my >> ysh),
                                 pic_width >> 1, pic_height >> (chroma_idc == 1 /* yuv420 */));
        src_cr = sl->edge_emu_buffer;
    }
    chroma_op(dest_cr, src_cr, sl->mb_uvlinesize, height >> (chroma_idc == 1 /* yuv420 */),
              mx & 7, ((unsigned)my << (chroma_idc == 2 /* yuv422 */)) & 7);
}

static av_always_inline void mc_part_std(const H264Context *h, H264SliceContext *sl,
                                         int n, int square,
                                         int height, int delta,
                                         uint8_t *dest_y, uint8_t *dest_cb,
                                         uint8_t *dest_cr,
                                         int x_offset, int y_offset,
                                         const qpel_mc_func *qpix_put,
                                         h264_chroma_mc_func chroma_put,
                                         const qpel_mc_func *qpix_avg,
                                         h264_chroma_mc_func chroma_avg,
                                         int list0, int list1,
                                         int pixel_shift, int chroma_idc)
{
    const qpel_mc_func *qpix_op   = qpix_put;
    h264_chroma_mc_func chroma_op = chroma_put;

    dest_y += (2 * x_offset << pixel_shift) + 2 * y_offset * sl->mb_linesize;
    if (chroma_idc == 3 /* yuv444 */) {
        dest_cb += (2 * x_offset << pixel_shift) + 2 * y_offset * sl->mb_linesize;
        dest_cr += (2 * x_offset << pixel_shift) + 2 * y_offset * sl->mb_linesize;
    } else if (chroma_idc == 2 /* yuv422 */) {
        dest_cb += (x_offset << pixel_shift) + 2 * y_offset * sl->mb_uvlinesize;
        dest_cr += (x_offset << pixel_shift) + 2 * y_offset * sl->mb_uvlinesize;
    } else { /* yuv420 */
        dest_cb += (x_offset << pixel_shift) + y_offset * sl->mb_uvlinesize;
        dest_cr += (x_offset << pixel_shift) + y_offset * sl->mb_uvlinesize;
    }
    x_offset += 8 * sl->mb_x;
    y_offset += 8 * (sl->mb_y >> MB_FIELD(sl));

    if (list0) {
        H264Ref *ref = &sl->ref_list[0][sl->ref_cache[0][scan8[n]]];
        mc_dir_part(h, sl, ref, n, square, height, delta, 0,
                    dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_op, chroma_op, pixel_shift, chroma_idc);

        qpix_op   = qpix_avg;
        chroma_op = chroma_avg;
    }

    if (list1) {
        H264Ref *ref = &sl->ref_list[1][sl->ref_cache[1][scan8[n]]];
        mc_dir_part(h, sl, ref, n, square, height, delta, 1,
                    dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_op, chroma_op, pixel_shift, chroma_idc);
    }
}

static av_always_inline void mc_part_weighted(const H264Context *h, H264SliceContext *sl,
                                              int n, int square,
                                              int height, int delta,
                                              uint8_t *dest_y, uint8_t *dest_cb,
                                              uint8_t *dest_cr,
                                              int x_offset, int y_offset,
                                              const qpel_mc_func *qpix_put,
                                              h264_chroma_mc_func chroma_put,
                                              h264_weight_func luma_weight_op,
                                              h264_weight_func chroma_weight_op,
                                              h264_biweight_func luma_weight_avg,
                                              h264_biweight_func chroma_weight_avg,
                                              int list0, int list1,
                                              int pixel_shift, int chroma_idc)
{
    int chroma_height;

    dest_y += (2 * x_offset << pixel_shift) + 2 * y_offset * sl->mb_linesize;
    if (chroma_idc == 3 /* yuv444 */) {
        chroma_height     = height;
        chroma_weight_avg = luma_weight_avg;
        chroma_weight_op  = luma_weight_op;
        dest_cb += (2 * x_offset << pixel_shift) + 2 * y_offset * sl->mb_linesize;
        dest_cr += (2 * x_offset << pixel_shift) + 2 * y_offset * sl->mb_linesize;
    } else if (chroma_idc == 2 /* yuv422 */) {
        chroma_height = height;
        dest_cb      += (x_offset << pixel_shift) + 2 * y_offset * sl->mb_uvlinesize;
        dest_cr      += (x_offset << pixel_shift) + 2 * y_offset * sl->mb_uvlinesize;
    } else { /* yuv420 */
        chroma_height = height >> 1;
        dest_cb      += (x_offset << pixel_shift) + y_offset * sl->mb_uvlinesize;
        dest_cr      += (x_offset << pixel_shift) + y_offset * sl->mb_uvlinesize;
    }
    x_offset += 8 * sl->mb_x;
    y_offset += 8 * (sl->mb_y >> MB_FIELD(sl));

    if (list0 && list1) {
        /* don't optimize for luma-only case, since B-frames usually
         * use implicit weights => chroma too. */
        uint8_t *tmp_cb = sl->bipred_scratchpad;
        uint8_t *tmp_cr = sl->bipred_scratchpad + (8 << pixel_shift + (chroma_idc == 3));
        uint8_t *tmp_y  = sl->bipred_scratchpad + 16 * sl->mb_uvlinesize;
        int refn0       = sl->ref_cache[0][scan8[n]];
        int refn1       = sl->ref_cache[1][scan8[n]];

        mc_dir_part(h, sl, &sl->ref_list[0][refn0], n, square, height, delta, 0,
                    dest_y, dest_cb, dest_cr,
                    x_offset, y_offset, qpix_put, chroma_put,
                    pixel_shift, chroma_idc);
        mc_dir_part(h, sl, &sl->ref_list[1][refn1], n, square, height, delta, 1,
                    tmp_y, tmp_cb, tmp_cr,
                    x_offset, y_offset, qpix_put, chroma_put,
                    pixel_shift, chroma_idc);

        if (sl->pwt.use_weight == 2) {
            int weight0 = sl->pwt.implicit_weight[refn0][refn1][sl->mb_y & 1];
            int weight1 = 64 - weight0;
            luma_weight_avg(dest_y, tmp_y, sl->mb_linesize,
                            height, 5, weight0, weight1, 0);
            if (!CONFIG_GRAY || !(h->flags & AV_CODEC_FLAG_GRAY)) {
                chroma_weight_avg(dest_cb, tmp_cb, sl->mb_uvlinesize,
                                  chroma_height, 5, weight0, weight1, 0);
                chroma_weight_avg(dest_cr, tmp_cr, sl->mb_uvlinesize,
                                  chroma_height, 5, weight0, weight1, 0);
            }
        } else {
            luma_weight_avg(dest_y, tmp_y, sl->mb_linesize, height,
                            sl->pwt.luma_log2_weight_denom,
                            sl->pwt.luma_weight[refn0][0][0],
                            sl->pwt.luma_weight[refn1][1][0],
                            sl->pwt.luma_weight[refn0][0][1] +
                            sl->pwt.luma_weight[refn1][1][1]);
            if (!CONFIG_GRAY || !(h->flags & AV_CODEC_FLAG_GRAY)) {
                chroma_weight_avg(dest_cb, tmp_cb, sl->mb_uvlinesize, chroma_height,
                                  sl->pwt.chroma_log2_weight_denom,
                                  sl->pwt.chroma_weight[refn0][0][0][0],
                                  sl->pwt.chroma_weight[refn1][1][0][0],
                                  sl->pwt.chroma_weight[refn0][0][0][1] +
                                  sl->pwt.chroma_weight[refn1][1][0][1]);
                chroma_weight_avg(dest_cr, tmp_cr, sl->mb_uvlinesize, chroma_height,
                                  sl->pwt.chroma_log2_weight_denom,
                                  sl->pwt.chroma_weight[refn0][0][1][0],
                                  sl->pwt.chroma_weight[refn1][1][1][0],
                                  sl->pwt.chroma_weight[refn0][0][1][1] +
                                  sl->pwt.chroma_weight[refn1][1][1][1]);
            }
        }
    } else {
        int list     = list1 ? 1 : 0;
        int refn     = sl->ref_cache[list][scan8[n]];
        H264Ref *ref = &sl->ref_list[list][refn];
        mc_dir_part(h, sl, ref, n, square, height, delta, list,
                    dest_y, dest_cb, dest_cr, x_offset, y_offset,
                    qpix_put, chroma_put, pixel_shift, chroma_idc);

        luma_weight_op(dest_y, sl->mb_linesize, height,
                       sl->pwt.luma_log2_weight_denom,
                       sl->pwt.luma_weight[refn][list][0],
                       sl->pwt.luma_weight[refn][list][1]);
        if (!CONFIG_GRAY || !(h->flags & AV_CODEC_FLAG_GRAY)) {
            if (sl->pwt.use_weight_chroma) {
                chroma_weight_op(dest_cb, sl->mb_uvlinesize, chroma_height,
                                 sl->pwt.chroma_log2_weight_denom,
                                 sl->pwt.chroma_weight[refn][list][0][0],
                                 sl->pwt.chroma_weight[refn][list][0][1]);
                chroma_weight_op(dest_cr, sl->mb_uvlinesize, chroma_height,
                                 sl->pwt.chroma_log2_weight_denom,
                                 sl->pwt.chroma_weight[refn][list][1][0],
                                 sl->pwt.chroma_weight[refn][list][1][1]);
            }
        }
    }
}

static av_always_inline void prefetch_motion(const H264Context *h, H264SliceContext *sl,
                                             int list, int pixel_shift,
                                             int chroma_idc)
{
    /* fetch pixels for estimated mv 4 macroblocks ahead
     * optimized for 64byte cache lines */
    const int refn = sl->ref_cache[list][scan8[0]];
    if (refn >= 0) {
        const int mx  = (sl->mv_cache[list][scan8[0]][0] >> 2) + 16 * sl->mb_x + 8;
        const int my  = (sl->mv_cache[list][scan8[0]][1] >> 2) + 16 * sl->mb_y;
        uint8_t **src = sl->ref_list[list][refn].data;
        int off       =  mx * (1<< pixel_shift) +
                        (my + (sl->mb_x & 3) * 4) * sl->mb_linesize +
                        (64 << pixel_shift);
#if ARCH_PPC && !defined(_ARCH_PWR4)
        /* The scheme below spreads one macroblock's worth of prefetch over
         * the next 4 (luma) or 8 (chroma) macroblocks, assuming a 64-byte
         * line covers four macroblock columns at once.  A 7450 (G4) has
         * 32-byte lines, so each touch brings in a quarter of what it does
         * on the CPU the code was tuned for -- and the rolling scheme only
         * pays while the motion vector stays put across those macroblocks.
         * Motion compensation there is memory-latency bound, so fetch the
         * whole block instead: 32 dcbt per macroblock and list, 1 KiB into
         * a 32 KiB L1, still issued four macroblocks ahead.
         * From PowerVLC (github.com/Olsro/powervlc, contrib/src/ffmpeg):
         * 1080p24 High 14.78 -> 14.92 fps (+1.0%) on a 1.42 GHz 7447A,
         * 720p unchanged.  Kept off the 970 (G5, _ARCH_PWR4): its 128-byte
         * lines make the upstream scheme cheaper, and there is no G5
         * measurement for this one yet. */
        off = mx * (1 << pixel_shift) + my * sl->mb_linesize + (64 << pixel_shift);
        h->vdsp.prefetch(src[0] + off, sl->linesize, 16);
        if (chroma_idc == 3 /* yuv444 */) {
            h->vdsp.prefetch(src[1] + off, sl->linesize, 16);
            h->vdsp.prefetch(src[2] + off, sl->linesize, 16);
        } else {
            off = ((mx >> 1) + 64) * (1 << pixel_shift) + (my >> 1) * sl->uvlinesize;
            h->vdsp.prefetch(src[1] + off, sl->uvlinesize, 8);
            h->vdsp.prefetch(src[2] + off, sl->uvlinesize, 8);
        }
#else
        h->vdsp.prefetch(src[0] + off, sl->linesize, 4);
        if (chroma_idc == 3 /* yuv444 */) {
            h->vdsp.prefetch(src[1] + off, sl->linesize, 4);
            h->vdsp.prefetch(src[2] + off, sl->linesize, 4);
        } else {
            off= ((mx>>1)+64) * (1<<pixel_shift) + ((my>>1) + (sl->mb_x&7))*sl->uvlinesize;
            h->vdsp.prefetch(src[1] + off, src[2] - src[1], 2);
        }
#endif
    }
}

static av_always_inline void xchg_mb_border(const H264Context *h, H264SliceContext *sl,
                                            uint8_t *src_y,
                                            uint8_t *src_cb, uint8_t *src_cr,
                                            int linesize, int uvlinesize,
                                            int xchg, int chroma444,
                                            int simple, int pixel_shift)
{
    int deblock_topleft;
    int deblock_top;
    int top_idx = 1;
    uint8_t *top_border_m1;
    uint8_t *top_border;

    if (!simple && FRAME_MBAFF(h)) {
        if (sl->mb_y & 1) {
            if (!MB_MBAFF(sl))
                return;
        } else {
            top_idx = MB_MBAFF(sl) ? 0 : 1;
        }
    }

    if (sl->deblocking_filter == 2) {
        deblock_topleft = h->slice_table[sl->mb_xy - 1 - h->mb_stride] == sl->slice_num;
        deblock_top     = sl->top_type;
    } else {
        deblock_topleft = (sl->mb_x > 0);
        deblock_top     = (sl->mb_y > !!MB_FIELD(sl));
    }

    src_y  -= linesize   + 1 + pixel_shift;
    src_cb -= uvlinesize + 1 + pixel_shift;
    src_cr -= uvlinesize + 1 + pixel_shift;

    top_border_m1 = sl->top_borders[top_idx][sl->mb_x - 1];
    top_border    = sl->top_borders[top_idx][sl->mb_x];

#define XCHG(a, b, xchg)                        \
    if (pixel_shift) {                          \
        if (xchg) {                             \
            AV_SWAP64(b + 0, a + 0);            \
            AV_SWAP64(b + 8, a + 8);            \
        } else {                                \
            AV_COPY128(b, a);                   \
        }                                       \
    } else if (xchg)                            \
        AV_SWAP64(b, a);                        \
    else                                        \
        AV_COPY64(b, a);

    if (deblock_top) {
        if (deblock_topleft) {
            XCHG(top_border_m1 + (8 << pixel_shift),
                 src_y - (7 << pixel_shift), 1);
        }
        XCHG(top_border + (0 << pixel_shift), src_y + (1 << pixel_shift), xchg);
        XCHG(top_border + (8 << pixel_shift), src_y + (9 << pixel_shift), 1);
        if (sl->mb_x + 1 < h->mb_width) {
            XCHG(sl->top_borders[top_idx][sl->mb_x + 1],
                 src_y + (17 << pixel_shift), 1);
        }
        if (simple || !CONFIG_GRAY || !(h->flags & AV_CODEC_FLAG_GRAY)) {
            if (chroma444) {
                if (deblock_topleft) {
                    XCHG(top_border_m1 + (24 << pixel_shift), src_cb - (7 << pixel_shift), 1);
                    XCHG(top_border_m1 + (40 << pixel_shift), src_cr - (7 << pixel_shift), 1);
                }
                XCHG(top_border + (16 << pixel_shift), src_cb + (1 << pixel_shift), xchg);
                XCHG(top_border + (24 << pixel_shift), src_cb + (9 << pixel_shift), 1);
                XCHG(top_border + (32 << pixel_shift), src_cr + (1 << pixel_shift), xchg);
                XCHG(top_border + (40 << pixel_shift), src_cr + (9 << pixel_shift), 1);
                if (sl->mb_x + 1 < h->mb_width) {
                    XCHG(sl->top_borders[top_idx][sl->mb_x + 1] + (16 << pixel_shift), src_cb + (17 << pixel_shift), 1);
                    XCHG(sl->top_borders[top_idx][sl->mb_x + 1] + (32 << pixel_shift), src_cr + (17 << pixel_shift), 1);
                }
            } else {
                if (deblock_topleft) {
                    XCHG(top_border_m1 + (16 << pixel_shift), src_cb - (7 << pixel_shift), 1);
                    XCHG(top_border_m1 + (24 << pixel_shift), src_cr - (7 << pixel_shift), 1);
                }
                XCHG(top_border + (16 << pixel_shift), src_cb + 1 + pixel_shift, 1);
                XCHG(top_border + (24 << pixel_shift), src_cr + 1 + pixel_shift, 1);
            }
        }
    }
}

static av_always_inline int dctcoef_get(int16_t *mb, int high_bit_depth,
                                        int index)
{
    if (high_bit_depth) {
        return AV_RN32A(((int32_t *)mb) + index);
    } else
        return AV_RN16A(mb + index);
}

static av_always_inline void dctcoef_set(int16_t *mb, int high_bit_depth,
                                         int index, int value)
{
    if (high_bit_depth) {
        AV_WN32A(((int32_t *)mb) + index, value);
    } else
        AV_WN16A(mb + index, value);
}

static av_always_inline void hl_decode_mb_predict_luma(const H264Context *h,
                                                       H264SliceContext *sl,
                                                       int mb_type, int simple,
                                                       int transform_bypass,
                                                       int pixel_shift,
                                                       const int *block_offset,
                                                       int linesize,
                                                       uint8_t *dest_y, int p)
{
    void (*idct_add)(uint8_t *dst, int16_t *block, int stride);
    void (*idct_dc_add)(uint8_t *dst, int16_t *block, int stride);
    int i;
    int qscale = p == 0 ? sl->qscale : sl->chroma_qp[p - 1];
    block_offset += 16 * p;
    if (IS_INTRA4x4(mb_type)) {
        if (IS_8x8DCT(mb_type)) {
            if (transform_bypass) {
                idct_dc_add =
                idct_add    = h->h264dsp.h264_add_pixels8_clear;
            } else {
                idct_dc_add = h->h264dsp.h264_idct8_dc_add;
                idct_add    = h->h264dsp.h264_idct8_add;
            }
            for (i = 0; i < 16; i += 4) {
                uint8_t *const ptr = dest_y + block_offset[i];
                const int dir      = sl->intra4x4_pred_mode_cache[scan8[i]];
                if (transform_bypass && h->ps.sps->profile_idc == 244 && dir <= 1) {
                    if (h->x264_build < 151U) {
                        h->hpc.pred8x8l_add[dir](ptr, sl->mb + (i * 16 + p * 256 << pixel_shift), linesize);
                    } else
                        h->hpc.pred8x8l_filter_add[dir](ptr, sl->mb + (i * 16 + p * 256 << pixel_shift),
                                                        (sl-> topleft_samples_available << i) & 0x8000,
                                                        (sl->topright_samples_available << i) & 0x4000, linesize);
                } else {
                    const int nnz = sl->non_zero_count_cache[scan8[i + p * 16]];
                    h->hpc.pred8x8l[dir](ptr, (sl->topleft_samples_available << i) & 0x8000,
                                         (sl->topright_samples_available << i) & 0x4000, linesize);
                    if (nnz) {
                        if (nnz == 1 && dctcoef_get(sl->mb, pixel_shift, i * 16 + p * 256))
                            idct_dc_add(ptr, sl->mb + (i * 16 + p * 256 << pixel_shift), linesize);
                        else
                            idct_add(ptr, sl->mb + (i * 16 + p * 256 << pixel_shift), linesize);
                    }
                }
            }
        } else {
            if (transform_bypass) {
                idct_dc_add  =
                idct_add     = h->h264dsp.h264_add_pixels4_clear;
            } else {
                idct_dc_add = h->h264dsp.h264_idct_dc_add;
                idct_add    = h->h264dsp.h264_idct_add;
            }
            for (i = 0; i < 16; i++) {
                uint8_t *const ptr = dest_y + block_offset[i];
                const int dir      = sl->intra4x4_pred_mode_cache[scan8[i]];

                if (transform_bypass && h->ps.sps->profile_idc == 244 && dir <= 1) {
                    h->hpc.pred4x4_add[dir](ptr, sl->mb + (i * 16 + p * 256 << pixel_shift), linesize);
                } else {
                    uint8_t *topright;
                    int nnz, tr;
                    uint64_t tr_high;
                    if (dir == DIAG_DOWN_LEFT_PRED || dir == VERT_LEFT_PRED) {
                        const int topright_avail = (sl->topright_samples_available << i) & 0x8000;
                        av_assert2(sl->mb_y || linesize <= block_offset[i]);
                        if (!topright_avail) {
                            if (pixel_shift) {
                                tr_high  = ((uint16_t *)ptr)[3 - linesize / 2] * 0x0001000100010001ULL;
                                topright = (uint8_t *)&tr_high;
                            } else {
                                tr       = ptr[3 - linesize] * 0x01010101u;
                                topright = (uint8_t *)&tr;
                            }
                        } else
                            topright = ptr + (4 << pixel_shift) - linesize;
                    } else
                        topright = NULL;

                    h->hpc.pred4x4[dir](ptr, topright, linesize);
                    nnz = sl->non_zero_count_cache[scan8[i + p * 16]];
                    if (nnz) {
                        if (nnz == 1 && dctcoef_get(sl->mb, pixel_shift, i * 16 + p * 256))
                            idct_dc_add(ptr, sl->mb + (i * 16 + p * 256 << pixel_shift), linesize);
                        else
                            idct_add(ptr, sl->mb + (i * 16 + p * 256 << pixel_shift), linesize);
                    }
                }
            }
        }
    } else {
        h->hpc.pred16x16[sl->intra16x16_pred_mode](dest_y, linesize);
        if (sl->non_zero_count_cache[scan8[LUMA_DC_BLOCK_INDEX + p]]) {
            if (!transform_bypass)
                h->h264dsp.h264_luma_dc_dequant_idct(sl->mb + (p * 256 << pixel_shift),
                                                     sl->mb_luma_dc[p],
                                                     h->ps.pps->dequant4_coeff[p][qscale][0]);
            else {
                static const uint8_t dc_mapping[16] = {
                     0 * 16,  1 * 16,  4 * 16,  5 * 16,
                     2 * 16,  3 * 16,  6 * 16,  7 * 16,
                     8 * 16,  9 * 16, 12 * 16, 13 * 16,
                    10 * 16, 11 * 16, 14 * 16, 15 * 16
                };
                for (i = 0; i < 16; i++)
                    dctcoef_set(sl->mb + (p * 256 << pixel_shift),
                                pixel_shift, dc_mapping[i],
                                dctcoef_get(sl->mb_luma_dc[p],
                                            pixel_shift, i));
            }
        }
    }
}

int g_x1900_debug_frameno = -1; /* diagnostic only - correlate MB coordinates across real
                               * decode order. Deliberately NOT static: this is a process-
                               * lifetime counter, but a test harness that runs multiple
                               * independent decode_to_frame() calls (fresh AVCodecContext
                               * each time - e.g. this project's own live-vs-reference
                               * comparison) needs to reset it back to -1 at the start of each
                               * one, or frame numbers from two separate decode runs won't be
                               * comparable (each real frame's own decode-order index resets,
                               * but this counter otherwise wouldn't). See gpu_live_decode_test.c's
                               * decode_to_frame(). */

static av_always_inline void hl_decode_mb_idct_luma(const H264Context *h, H264SliceContext *sl,
                                                    int mb_type, int simple,
                                                    int transform_bypass,
                                                    int pixel_shift,
                                                    const int *block_offset,
                                                    int linesize,
                                                    uint8_t *dest_y, int p)
{
    void (*idct_add)(uint8_t *dst, int16_t *block, int stride);
    int i;
    block_offset += 16 * p;
    if (!IS_INTRA4x4(mb_type)) {
        if (IS_INTRA16x16(mb_type)) {
            if (transform_bypass) {
                if (h->ps.sps->profile_idc == 244 &&
                    (sl->intra16x16_pred_mode == VERT_PRED8x8 ||
                     sl->intra16x16_pred_mode == HOR_PRED8x8)) {
                    h->hpc.pred16x16_add[sl->intra16x16_pred_mode](dest_y, block_offset,
                                                                   sl->mb + (p * 256 << pixel_shift),
                                                                   linesize);
                } else {
                    for (i = 0; i < 16; i++)
                        if (sl->non_zero_count_cache[scan8[i + p * 16]] ||
                            dctcoef_get(sl->mb, pixel_shift, i * 16 + p * 256))
                            h->h264dsp.h264_add_pixels4_clear(dest_y + block_offset[i],
                                                              sl->mb + (i * 16 + p * 256 << pixel_shift),
                                                              linesize);
                }
            } else {
                h->h264dsp.h264_idct_add16intra(dest_y, block_offset,
                                                sl->mb + (p * 256 << pixel_shift),
                                                linesize,
                                                sl->non_zero_count_cache + p * 5 * 8);
            }
        } else if (sl->cbp & 15) {
            if (transform_bypass) {
                const int di = IS_8x8DCT(mb_type) ? 4 : 1;
                idct_add = IS_8x8DCT(mb_type) ? h->h264dsp.h264_add_pixels8_clear
                    : h->h264dsp.h264_add_pixels4_clear;
                for (i = 0; i < 16; i += di)
                    if (sl->non_zero_count_cache[scan8[i + p * 16]])
                        idct_add(dest_y + block_offset[i],
                                 sl->mb + (i * 16 + p * 256 << pixel_shift),
                                 linesize);
            } else {
                if (IS_8x8DCT(mb_type))
                    h->h264dsp.h264_idct8_add4(dest_y, block_offset,
                                               sl->mb + (p * 256 << pixel_shift),
                                               linesize,
                                               sl->non_zero_count_cache + p * 5 * 8);
                else {
                    if (getenv("DEBUG_REAL_MB16") && sl->mb_x == atoi(getenv("DEBUG_REAL_MB16")) &&
                        sl->mb_y == (getenv("DEBUG_REAL_MB16_2") ? atoi(getenv("DEBUG_REAL_MB16_2")) : -999)) {
                        static int hit = 0;
                        hit++;
                        fprintf(stderr, "REAL P16 MB(%d,%d) hit#%d frameno=%d: cbp=0x%x mb_type=0x%x\n",
                                sl->mb_x, sl->mb_y, hit, g_x1900_debug_frameno, sl->cbp, mb_type);
                        fprintf(stderr, "  nnz(scan8[i],p=%d): ", p);
                        for (int ii = 0; ii < 16; ii++) fprintf(stderr, "%d ", sl->non_zero_count_cache[scan8[ii + p * 16]]);
                        fprintf(stderr, "\n  raw coeffs blk5 (scan8 idx %d): ", 5);
                        int16_t *blk5 = sl->mb + ((5 * 16 + p * 256) << pixel_shift);
                        for (int cc = 0; cc < 16; cc++) fprintf(stderr, "%d ", blk5[cc]);
                        fprintf(stderr, "\n");
                    }
                    h->h264dsp.h264_idct_add16(dest_y, block_offset,
                                               sl->mb + (p * 256 << pixel_shift),
                                               linesize,
                                               sl->non_zero_count_cache + p * 5 * 8);
                }
            }
        }
    }
}

#define BITS   8
#define SIMPLE 1
#include "h264_mb_template.c"

#undef  BITS
#define BITS   16
#include "h264_mb_template.c"

#undef  SIMPLE
#define SIMPLE 0
#include "h264_mb_template.c"

/* One-off item-3 design-pass pre-flight census (see DEBUG_PART_CENSUS below) -
 * not a permanent tool. */
static long g_part_census_16x16, g_part_census_16x8, g_part_census_8x16,
            g_part_census_8x8_whole, g_part_census_8x8_8x4, g_part_census_8x8_4x8,
            g_part_census_8x8_4x4, g_part_census_other;
static int g_part_census_atexit_registered;
static void print_part_census(void) {
    fprintf(stderr, "PART CENSUS: 16x16=%ld 16x8=%ld 8x16=%ld "
            "8x8(whole)=%ld 8x8(8x4)=%ld 8x8(4x8)=%ld 8x8(4x4)=%ld other=%ld\n",
            g_part_census_16x16, g_part_census_16x8, g_part_census_8x16,
            g_part_census_8x8_whole, g_part_census_8x8_8x4, g_part_census_8x8_4x8,
            g_part_census_8x8_4x4, g_part_census_other);
}

void ff_h264_hl_decode_mb(const H264Context *h, H264SliceContext *sl)
{
    const int mb_xy   = sl->mb_xy;
    const int mb_type = h->cur_pic.mb_type[mb_xy];
    int is_complex;

    if (sl->mb_x == 0 && sl->mb_y == 0) {
        g_x1900_debug_frameno++;
        if (getenv("DEBUG_FRAME_BOUNDARY"))
            fprintf(stderr, "FRAME BOUNDARY: g_x1900_debug_frameno=%d frame_num=%d poc=%d slice_type=%d\n",
                    g_x1900_debug_frameno, sl->frame_num, h->cur_pic_ptr ? h->cur_pic_ptr->poc : -1, sl->slice_type);
    }

    /* x1900 GPU-decode hook: called with real, already entropy-decoded
     * per-MB data - see x1900_hook.h for why this call site, not
     * AVHWAccel, is the correct interception point.
     *
     * mv_cache/ref_cache are NOT indexed from 0 for a macroblock's first
     * sub-block - they use the scan8[] cache layout (a padded 8-wide grid
     * with a 2-cell border for neighbor-availability sentinels), so the
     * real first-sub-block offset is scan8[0]=12, not 0 (bug found and
     * fixed during Milestone 7: reading raw index 0 silently returned
     * border/padding cells, which read as all-zero regardless of the
     * real content's actual motion - not a content property). */
    if (getenv("DEBUG_PART_CENSUS")) {
        /* One-off census (item 3 design-pass pre-flight check, not a
         * permanent tool) - tallies real MC partition shapes across every
         * inter macroblock this decode sees, to confirm 16x8/8x16/8x8
         * content actually exists in this test clip before writing any
         * sub-8x8 reconstruction code against it. Printed via atexit(). */
        if (IS_INTER(mb_type)) {
            if (mb_type & MB_TYPE_16x16) g_part_census_16x16++;
            else if (mb_type & MB_TYPE_16x8) g_part_census_16x8++;
            else if (mb_type & MB_TYPE_8x16) g_part_census_8x16++;
            else if (mb_type & MB_TYPE_8x8) {
                for (int qi = 0; qi < 4; qi++) {
                    int smt = sl->sub_mb_type[qi];
                    if (smt & MB_TYPE_16x16) g_part_census_8x8_whole++;
                    else if (smt & MB_TYPE_16x8) g_part_census_8x8_8x4++;
                    else if (smt & MB_TYPE_8x16) g_part_census_8x8_4x8++;
                    else g_part_census_8x8_4x4++;
                }
            } else g_part_census_other++;
        }
        if (!g_part_census_atexit_registered) {
            g_part_census_atexit_registered = 1;
            atexit(print_part_census);
        }
    }

    {
        X1900MbInfo x1900_info;
        /* Use sl->mb_x/sl->mb_y directly - NOT "mb_xy % h->mb_width" /
         * "mb_xy / h->mb_width". mb_xy is sl->mb_x + sl->mb_y * h->mb_stride,
         * and mb_stride is mb_width+1 (one column of border padding), so
         * for every row beyond row 0 that modulo/division recovers the
         * WRONG mb_x/mb_y (systematically shifted, worse each row down -
         * row 1 read one column too far right, etc). This corrupted the
         * dest_y pointer built from it below: it silently pointed either
         * at a neighboring macroblock's data instead of this one's own,
         * or - for the first real integration test's captured MB - at
         * this exact macroblock's own not-yet-reconstructed pixels,
         * which read back as 0 since the hook fires before FFmpeg's own
         * reconstruction writes anything. sl->mb_x/mb_y are the real,
         * already-correct values the rest of this function (and
         * h264_mb_template.c's own dest_y) uses directly - use those. */
        x1900_info.mb_x = sl->mb_x;
        x1900_info.mb_y = sl->mb_y;
        x1900_info.mb_type = mb_type;
        x1900_info.qscale = sl->qscale;
        x1900_info.coeffs = sl->mb;
        x1900_info.nnz = sl->non_zero_count_cache;
        x1900_info.mv_l0 = (const int16_t *)sl->mv_cache[0][scan8[0]];
        x1900_info.ref_l0 = (const int8_t *)&sl->ref_cache[0][scan8[0]];
        /* Sub-8x8 partition support (item 3) - full underlying arrays,
         * NOT repointing mv_l0/ref_l0 above (see x1900_hook.h's comment
         * on why that would silently break existing P_Skip/P_16x16). */
        x1900_info.mv_l0_cache = (const int16_t *)sl->mv_cache[0];
        x1900_info.ref_l0_cache = (const int8_t *)sl->ref_cache[0];
        for (int qi = 0; qi < 4; qi++)
            x1900_info.sub_mb_type[qi] = sl->sub_mb_type[qi];
        x1900_info.luma_dc = sl->mb_luma_dc[0];
        x1900_info.luma_dc_qmul = h->ps.pps->dequant4_coeff[0][sl->qscale][0];
        /* PIXEL_SHIFT is 0 for 8-bit content (this project's whole scope -
         * see the plan's Scope section) - matches h264_mb_template.c's
         * own dest_y computation for that case exactly. */
        x1900_info.dest_y = h->cur_pic.f->data[0] +
            (x1900_info.mb_x + x1900_info.mb_y * sl->linesize) * 16;
        x1900_info.linesize = sl->linesize;
        x1900_info.intra16x16_pred_mode = sl->intra16x16_pred_mode;

        /* Chroma - always valid to compute (cheap), only meaningful for
         * intra macroblocks. Matches h264_mb_template.c's own dest_cb/
         * dest_cr computation exactly (block_h=8 for 4:2:0 progressive). */
        x1900_info.dest_cb = h->cur_pic.f->data[1] +
            x1900_info.mb_x * 8 + x1900_info.mb_y * sl->uvlinesize * 8;
        x1900_info.dest_cr = h->cur_pic.f->data[2] +
            x1900_info.mb_x * 8 + x1900_info.mb_y * sl->uvlinesize * 8;
        x1900_info.uvlinesize = sl->uvlinesize;
        x1900_info.chroma_pred_mode = sl->chroma_pred_mode;
        x1900_info.chroma_dc_qmul[0] = h->ps.pps->dequant4_coeff[IS_INTRA(mb_type) ? 1 : 4][sl->chroma_qp[0]][0];
        x1900_info.chroma_dc_qmul[1] = h->ps.pps->dequant4_coeff[IS_INTRA(mb_type) ? 2 : 5][sl->chroma_qp[1]][0];

        /* See x1900_hook.h's comment on these fields for why: reading
         * top-neighbor context straight out of the live buffer is wrong
         * once a row's own deblocking has already run (real FFmpeg
         * avoids this via xchg_mb_border(), which our hook fires before
         * it ever gets a chance to run). top_idx is always 1 for
         * non-MBAFF/progressive content (h264_mb.c's own xchg_mb_border,
         * "int top_idx = 1;" changed only inside a FRAME_MBAFF branch). */
        x1900_info.top_border_here = sl->top_borders[1][x1900_info.mb_x];
        x1900_info.top_border_left = x1900_info.mb_x > 0 ?
            sl->top_borders[1][x1900_info.mb_x - 1] : NULL;
        x1900_info.cbp = sl->cbp;
        x1900_info.mb_width = h->mb_width;
        x1900_info.mb_height = h->mb_height;
        x1900_info.slice_type_nos = sl->slice_type_nos;

        /* Inter (P-slice, list-0, single-reference) support - see
         * x1900_hook.h's comment on these fields. ref_cache[0][scan8[0]]
         * is only meaningful when this macroblock actually uses list 0
         * (IS_INTER); reading it for an intra macroblock would be
         * garbage, so gate on that first. */
        if (IS_INTER(mb_type)) {
            int ref_idx = sl->ref_cache[0][scan8[0]];
            if (ref_idx >= 0 && ref_idx < 48 && sl->ref_list[0][ref_idx].data[0]) {
                H264Ref *ref = &sl->ref_list[0][ref_idx];
                if (getenv("DEBUG_REF_MB") && sl->mb_x == atoi(getenv("DEBUG_REF_MB")) &&
                    sl->mb_y == (getenv("DEBUG_REF_MB2") ? atoi(getenv("DEBUG_REF_MB2")) : -999)) {
                    int ref_idx1 = sl->ref_cache[1][scan8[0]];
                    int have_l1 = ref_idx1 >= 0 && ref_idx1 < 48 && sl->ref_list[1][ref_idx1].data[0];
                    fprintf(stderr, "REF MB(%d,%d) frameno=%d: mb_type=0x%x slice_type_nos=%d "
                            "L0 ref_idx=%d ref_poc=%d L1 ref_idx=%d ref_poc=%d cur_poc=%d numref0=%d numref1=%d\n",
                            sl->mb_x, sl->mb_y, g_x1900_debug_frameno, mb_type, sl->slice_type_nos,
                            ref_idx, ref->poc,
                            have_l1 ? ref_idx1 : -1, have_l1 ? sl->ref_list[1][ref_idx1].poc : -1,
                            h->cur_pic_ptr ? h->cur_pic_ptr->poc : -1, sl->ref_count[0], sl->ref_count[1]);
                }
                x1900_info.ref_y  = ref->data[0];
                x1900_info.ref_cb = ref->data[1];
                x1900_info.ref_cr = ref->data[2];
                x1900_info.ref_linesize   = ref->linesize[0];
                x1900_info.ref_uvlinesize = ref->linesize[1];
            } else {
                x1900_info.ref_y = x1900_info.ref_cb = x1900_info.ref_cr = NULL;
            }
        } else {
            x1900_info.ref_y = x1900_info.ref_cb = x1900_info.ref_cr = NULL;
        }

        if (getenv("DEBUG_REAL_MB16") && sl->mb_x == atoi(getenv("DEBUG_REAL_MB16")) &&
            sl->mb_y == (getenv("DEBUG_REAL_MB16_2") ? atoi(getenv("DEBUG_REAL_MB16_2")) : -999))
            fprintf(stderr, "PRE-HOOK MB(%d,%d) frameno=%d: sl->mb[80..95]="
                    "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                    sl->mb_x, sl->mb_y, g_x1900_debug_frameno,
                    sl->mb[80],sl->mb[81],sl->mb[82],sl->mb[83],sl->mb[84],sl->mb[85],sl->mb[86],sl->mb[87],
                    sl->mb[88],sl->mb[89],sl->mb[90],sl->mb[91],sl->mb[92],sl->mb[93],sl->mb[94],sl->mb[95]);

        if (ff_x1900_call_mb_hook(&x1900_info)) {
            /* Real bug found+fixed while adding P_16x16-with-residual:
             * sl->mb is a REUSED per-slice scratch buffer (16*48*2
             * int16_t, one slot per block, shared across every macroblock
             * in the slice) - decode_residual() (h264_cavlc.c/h264_cabac.c)
             * never writes explicit zeros to a block's "gap" positions
             * between its own nonzero coefficients, relying entirely on
             * whoever consumed this exact scratch slot LAST having zeroed
             * it back out. Real FFmpeg's own consumers do exactly that -
             * ff_h264_idct_add ends with `memset(block, 0, 16*sizeof(dctcoef))`,
             * ff_h264_idct_dc_add does `block[0] = 0` - both in
             * h264idct_template.c, both unconditionally, every time they
             * run. Those functions are called from hl_decode_mb_idct_luma/
             * ff_h264_idct_add8, i.e. exactly the normal-reconstruction
             * code path this early `return` skips whenever the hook takes
             * over. Skipping it leaves whatever this macroblock's own
             * entropy-decoded coefficients were sitting in sl->mb, which
             * then corrupts the NEXT macroblock that reuses this same
             * scratch slot and has fewer nonzero coefficients than what's
             * left behind - a large, real, and completely silent pixel
             * error (no GL error, no crash, cbp/nnz/mb_type all still
             * report correctly for the CURRENT macroblock; only a LATER
             * macroblock's reconstruction is wrong). Found by adding a
             * direct trace of sl->mb's raw contents at the hook call site
             * and comparing hook-live vs hook-never-installed decodes of
             * the identical macroblock/frame - the two decodes are
             * otherwise required to be bit-identical since entropy decode
             * never depends on pixel reconstruction, so any difference
             * here is definitionally a leftover-state bug, not content.
             * Fix: replicate the same "clear what I consumed" contract
             * ourselves for every block this project's hook code can
             * read (matches X1900MbInfo.coeffs's own documented layout -
             * luma blocks 0-15, Cb AC blocks 16-19, Cr AC blocks 32-35;
             * chroma DC lives AT position 0 of each chroma AC block, per
             * ff_h264_chroma_dc_dequant_idct's own addressing, so it's
             * already covered by this same range). Unconditional (not
             * gated on this macroblock's own nnz per block) is
             * deliberately simpler than replicating FFmpeg's exact
             * per-block conditional clearing and just as safe: a block
             * this macroblock's own nnz says has no data was never read
             * by anyone (ours or FFmpeg's) either way, so zeroing it
             * anyway is a harmless no-op, not a behavior change. 8-bit
             * only (no pixel_shift scaling) - matches this whole hook's
             * existing 8-bit-only scope. */
            memset(sl->mb, 0, 16 * 16 * sizeof(int16_t));
            memset(sl->mb + 16 * 16, 0, 4 * 16 * sizeof(int16_t));
            memset(sl->mb + 16 * 32, 0, 4 * 16 * sizeof(int16_t));
            return;
        }
    }

    is_complex = CONFIG_SMALL || sl->is_complex ||
                        IS_INTRA_PCM(mb_type) || sl->qscale == 0;

    if (CHROMA444(h)) {
        if (is_complex || h->pixel_shift)
            hl_decode_mb_444_complex(h, sl);
        else
            hl_decode_mb_444_simple_8(h, sl);
    } else if (is_complex) {
        hl_decode_mb_complex(h, sl);
    } else if (h->pixel_shift) {
        hl_decode_mb_simple_16(h, sl);
    } else
        hl_decode_mb_simple_8(h, sl);
}
