/* mp4-demux CLI (Milestone 2) - thin wrapper around mp4box.c */
#include "mp4box.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.mp4> [--dump-nals]\n", argv[0]);
        return 1;
    }
    int dump_nals = (argc > 2 && strcmp(argv[2], "--dump-nals") == 0);

    Mp4Movie mov;
    if (mp4_open(argv[1], &mov) != 0) {
        fprintf(stderr, "failed to parse %s (no H.264 track / bad file)\n", argv[1]);
        return 1;
    }

    printf("file: %s (%ld bytes)\n", argv[1], mov.file_size);
    printf("video: %dx%d, NAL length size=%d\n", mov.width, mov.height, mov.nal_length_size);
    if (mov.sps_len >= 4)
        printf("  SPS: profile_idc=%d level_idc=%d (%d bytes)\n", mov.sps[1], mov.sps[3], mov.sps_len);
    printf("  PPS: %d bytes\n", mov.pps_len);
    printf("sample_count=%u\n", mov.sample_count);

    for (uint32_t i = 0; i < mov.sample_count; i++) {
        Mp4Sample *s = &mov.samples[i];
        int verbose = (i < 5 || i >= mov.sample_count - 2);
        if (verbose)
            printf("sample %u: offset=%llu size=%u\n", i,
                   (unsigned long long)s->offset, s->size);
        if (dump_nals && s->offset + s->size <= (uint64_t)mov.file_size) {
            unsigned char *sp = mov.file_data + s->offset;
            uint32_t remaining = s->size;
            while (remaining > (uint32_t)mov.nal_length_size) {
                uint32_t nal_len = 0;
                for (int b = 0; b < mov.nal_length_size; b++)
                    nal_len = (nal_len << 8) | sp[b];
                sp += mov.nal_length_size;
                remaining -= mov.nal_length_size;
                if (nal_len == 0 || nal_len > remaining) break;
                if (verbose)
                    printf("    NAL: len=%u type=%d\n", nal_len, sp[0] & 0x1f);
                sp += nal_len;
                remaining -= nal_len;
            }
        }
    }

    mp4_free(&mov);
    return 0;
}
