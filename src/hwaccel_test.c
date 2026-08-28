/*
 * hwaccel-test: Milestone 5 skeleton. Installs the x1900 per-MB hook
 * (h264_mb.c's ff_h264_hl_decode_mb(), patched into our own FFmpeg build -
 * see x1900_hook.h for why this is the real interception point instead of
 * AVHWAccel) and logs what it receives for the first frame's macroblocks,
 * returning 0 every time so CPU reconstruction still runs normally -
 * proving the interception fires with real, correct per-MB data before
 * any GPU dispatch code is written.
 */

#include "mp4box.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavcodec/x1900_hook.h>

static int g_frame_count = 0;
static int g_mb_logged_this_frame = 0;

static int mb_hook(int mb_x, int mb_y, int mb_type, int qscale,
                    const int16_t *coeffs, const uint8_t *nnz,
                    const int16_t *mv_l0, const int8_t *ref_l0,
                    void *userdata) {
    (void)userdata;
    if (g_frame_count == 0 && g_mb_logged_this_frame < 8) {
        int nnz_total = 0;
        for (int i = 0; i < 15 * 8; i++) nnz_total += nnz[i] != 64 ? nnz[i] : 0;
        printf("  MB(%2d,%2d) type=0x%08x qscale=%d nnz_sum=%d "
               "coeff[0..3]=%d,%d,%d,%d mv_l0[0]=(%d,%d) ref_l0[0]=%d\n",
               mb_x, mb_y, mb_type, qscale, nnz_total,
               coeffs[0], coeffs[1], coeffs[2], coeffs[3],
               mv_l0[0], mv_l0[1], ref_l0[0]);
        g_mb_logged_this_frame++;
    }
    return 0; /* not handled - let FFmpeg's normal CPU path reconstruct it */
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.mp4>\n", argv[0]);
        return 1;
    }

    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) {
        fprintf(stderr, "failed to parse %s\n", argv[1]);
        return 1;
    }
    printf("input: %dx%d, %u samples\n", mov.width, mov.height, mov.sample_count);

    int avcc_len = 0;
    unsigned char *avcc = mp4_build_avcc(&mov, &avcc_len);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(avcc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)avcc_len);
    ctx->extradata_size = avcc_len;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 failed\n");
        return 1;
    }

    ff_x1900_set_mb_hook(mb_hook, NULL);
    printf("hook installed - decoding, watching only frame 0's first 8 MBs:\n\n");

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    int frames_out = 0;

    for (uint32_t i = 0; i < mov.sample_count && frames_out < 2; i++) {
        Mp4Sample *s = &mov.samples[i];
        if (av_new_packet(pkt, (int)s->size) < 0) break;
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            frames_out++;
            g_frame_count = frames_out; /* stop logging after frame 0 finishes */
            av_frame_unref(frame);
        }
    }

    printf("\ndecoded %d frame(s) with the hook installed - if the MB dump above\n"
           "shows real (non-zero-everywhere, varied) coefficients/types/MVs, the\n"
           "interception point is confirmed working.\n", frames_out);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    free(avcc);
    mp4_free(&mov);
    return 0;
}
