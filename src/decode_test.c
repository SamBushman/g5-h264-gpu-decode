/*
 * decode-test: Milestone 3 CPU-only reference decoder.
 *
 * Feeds our own mp4box demux output into FFmpeg's software H.264 decoder
 * (no hwaccel registered - that's Milestone 5+) and writes the first
 * decoded frame out as a PPM for a visual sanity check, plus per-frame
 * timing so this doubles as the --cpu-only speed baseline later.
 */

#include "mp4box.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>

static void write_ppm(AVFrame *f, const char *path) {
    FILE *out = fopen(path, "wb");
    if (!out) return;
    fprintf(out, "P6\n%d %d\n255\n", f->width, f->height);
    unsigned char *y = f->data[0], *u = f->data[1], *v = f->data[2];
    int ys = f->linesize[0], us = f->linesize[1], vs = f->linesize[2];
    for (int j = 0; j < f->height; j++) {
        for (int i = 0; i < f->width; i++) {
            int Y = y[j * ys + i];
            int U = u[(j / 2) * us + (i / 2)] - 128;
            int V = v[(j / 2) * vs + (i / 2)] - 128;
            int r = Y + (int)(1.402 * V);
            int g = Y - (int)(0.344136 * U) - (int)(0.714136 * V);
            int b = Y + (int)(1.772 * U);
            r = r < 0 ? 0 : (r > 255 ? 255 : r);
            g = g < 0 ? 0 : (g > 255 ? 255 : g);
            b = b < 0 ? 0 : (b > 255 ? 255 : b);
            unsigned char px[3] = {(unsigned char)r, (unsigned char)g, (unsigned char)b};
            fwrite(px, 1, 3, out);
        }
    }
    fclose(out);
}

static double now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.mp4> [out.ppm]\n", argv[0]);
        return 1;
    }
    const char *ppm_out = argc > 2 ? argv[2] : "first_frame.ppm";

    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) {
        fprintf(stderr, "failed to parse %s\n", argv[1]);
        return 1;
    }
    printf("input: %dx%d, %u samples, nal_length_size=%d\n",
           mov.width, mov.height, mov.sample_count, mov.nal_length_size);

    int avcc_len = 0;
    unsigned char *avcc = mp4_build_avcc(&mov, &avcc_len);

    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        fprintf(stderr, "h264 decoder not found in this FFmpeg build\n");
        return 1;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    ctx->extradata = (uint8_t *)av_mallocz(avcc_len + AV_INPUT_BUFFER_PADDING_SIZE);
    memcpy(ctx->extradata, avcc, (size_t)avcc_len);
    ctx->extradata_size = avcc_len;

    if (avcodec_open2(ctx, codec, NULL) < 0) {
        fprintf(stderr, "avcodec_open2 failed\n");
        return 1;
    }

    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    int frames_out = 0;
    int wrote_ppm = 0;
    double t_start = now_ms();

    for (uint32_t i = 0; i < mov.sample_count; i++) {
        Mp4Sample *s = &mov.samples[i];
        if (av_new_packet(pkt, (int)s->size) < 0) break;
        memcpy(pkt->data, mov.file_data + s->offset, s->size);

        int ret = avcodec_send_packet(ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "send_packet failed at sample %u: %d\n", i, ret);
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);

        while (avcodec_receive_frame(ctx, frame) == 0) {
            frames_out++;
            if (frames_out <= 3 || frames_out % 50 == 0)
                printf("frame %d: pts=%lld type=%c\n", frames_out,
                       (long long)frame->pts, av_get_picture_type_char(frame->pict_type));
            if (!wrote_ppm) {
                write_ppm(frame, ppm_out);
                wrote_ppm = 1;
                printf("  wrote %s\n", ppm_out);
            }
            av_frame_unref(frame);
        }
    }

    /* flush */
    avcodec_send_packet(ctx, NULL);
    while (avcodec_receive_frame(ctx, frame) == 0) {
        frames_out++;
        av_frame_unref(frame);
    }

    double t_end = now_ms();
    printf("decoded %d frames from %u samples in %.1f ms (%.2f ms/frame)\n",
           frames_out, mov.sample_count, t_end - t_start,
           frames_out ? (t_end - t_start) / frames_out : 0.0);

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    free(avcc);
    mp4_free(&mov);
    return 0;
}
