#include "mp4box.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/x1900_hook.h>

static int g_frame_idx = -1;
static int g_logged = 0;

static int g_nonzero_found = 0;

static int survey_hook(int mb_x, int mb_y, int mb_type, int qscale,
                        const int16_t *coeffs, const uint8_t *nnz,
                        const int16_t *mv_l0, const int8_t *ref_l0,
                        void *userdata) {
    (void)coeffs; (void)nnz; (void)qscale; (void)userdata;
    if (g_frame_idx >= 1 && (mv_l0[0] != 0 || mv_l0[1] != 0) && g_nonzero_found < 15) {
        printf("frame=%d MB(%2d,%2d) type=0x%08x mv=(%d,%d) fx=%d fy=%d ref=%d\n",
               g_frame_idx, mb_x, mb_y, mb_type, mv_l0[0], mv_l0[1],
               mv_l0[0] & 3, mv_l0[1] & 3, ref_l0[0]);
        g_nonzero_found++;
    }
    return 0;
}

int main(int argc, char **argv) {
    Mp4Movie mov;
    mp4_open(argv[1], &mov);
    int avcc_len = 0;
    unsigned char *avcc = mp4_build_avcc(&mov, &avcc_len);
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(avcc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)avcc_len);
    ctx->extradata_size = avcc_len;
    avcodec_open2(ctx, codec, NULL);
    ff_x1900_set_mb_hook(survey_hook, NULL);

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    g_frame_idx = 0;
    for (uint32_t i = 0; i < mov.sample_count && g_nonzero_found < 15; i++) {
        Mp4Sample *s = &mov.samples[i];
        av_new_packet(pkt, (int)s->size);
        memcpy(pkt->data, mov.file_data + s->offset, s->size);
        avcodec_send_packet(ctx, pkt);
        av_packet_unref(pkt);
        while (avcodec_receive_frame(ctx, frame) == 0) {
            g_frame_idx++;
            av_frame_unref(frame);
        }
    }
    printf("scanned to frame %d, found %d nonzero MVs\n", g_frame_idx, g_nonzero_found);
    return 0;
}
