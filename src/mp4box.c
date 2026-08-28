/*
 * mp4box: minimal ISO-BMFF (MP4) box parser, reusable module.
 * See mp4box.h. Extracted from the mp4-demux (Milestone 2) CLI so
 * Milestone 3+ (FFmpeg-based decode) can reuse the same parsing.
 */

#include "mp4box.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd_u32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint16_t rd_u16(const unsigned char *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static uint64_t rd_u64(const unsigned char *p) {
    return ((uint64_t)rd_u32(p) << 32) | (uint64_t)rd_u32(p + 4);
}

/* ---- intermediate state while walking, before flattening into Mp4Movie ---- */

typedef struct {
    uint32_t *sizes;
    uint32_t sample_count;
    uint32_t const_size;

    uint64_t *chunk_offsets;
    uint32_t chunk_count;

    uint32_t *stsc_first_chunk;
    uint32_t *stsc_spc;
    uint32_t stsc_count;

    unsigned char *sps;
    int sps_len;
    unsigned char *pps;
    int pps_len;
    int nal_length_size;

    int width, height;
    int found_video_track;
} ParseState;

static void parse_avcC(ParseState *st, const unsigned char *box, uint32_t box_size) {
    if (box_size < 7) return;
    st->nal_length_size = (box[4] & 0x03) + 1;
    int num_sps = box[5] & 0x1f;
    const unsigned char *p = box + 6;
    const unsigned char *end = box + box_size;
    for (int i = 0; i < num_sps && p + 2 <= end; i++) {
        int len = rd_u16(p);
        p += 2;
        if (p + len > end) break;
        if (i == 0) {
            st->sps = (unsigned char *)malloc((size_t)len);
            memcpy(st->sps, p, (size_t)len);
            st->sps_len = len;
        }
        p += len;
    }
    if (p >= end) return;
    int num_pps = *p++;
    for (int i = 0; i < num_pps && p + 2 <= end; i++) {
        int len = rd_u16(p);
        p += 2;
        if (p + len > end) break;
        if (i == 0) {
            st->pps = (unsigned char *)malloc((size_t)len);
            memcpy(st->pps, p, (size_t)len);
            st->pps_len = len;
        }
        p += len;
    }
}

static void walk_stbl(ParseState *st, unsigned char *p, unsigned char *end) {
    while (p + 8 <= end) {
        uint32_t size = rd_u32(p);
        char type[5] = {0};
        memcpy(type, p + 4, 4);
        unsigned char *box_start = p;
        unsigned char *box_body = p + 8;
        uint32_t box_total = size;
        if (size == 1) {
            box_total = (uint32_t)rd_u64(p + 8);
            box_body = p + 16;
        }
        if (size == 0 || box_start + box_total > end) break;

        if (strcmp(type, "stsd") == 0) {
            unsigned char *q = box_body + 8;
            if (q + 8 <= box_start + box_total) {
                uint32_t entry_size = rd_u32(q);
                char entry_type[5] = {0};
                memcpy(entry_type, q + 4, 4);
                if (strcmp(entry_type, "avc1") == 0 && q + entry_size <= box_start + box_total) {
                    unsigned char *e = q + 8;
                    if (e + 32 <= q + entry_size) {
                        st->width = rd_u16(e + 24);
                        st->height = rd_u16(e + 26);
                    }
                    unsigned char *inner = q + 8 + 78;
                    unsigned char *entry_end = q + entry_size;
                    while (inner + 8 <= entry_end) {
                        uint32_t isz = rd_u32(inner);
                        char itype[5] = {0};
                        memcpy(itype, inner + 4, 4);
                        if (isz == 0 || inner + isz > entry_end) break;
                        if (strcmp(itype, "avcC") == 0) {
                            parse_avcC(st, inner + 8, isz - 8);
                            st->found_video_track = 1;
                        }
                        inner += isz;
                    }
                }
            }
        } else if (strcmp(type, "stsz") == 0) {
            uint32_t const_size = rd_u32(box_body + 4);
            uint32_t count = rd_u32(box_body + 8);
            st->const_size = const_size;
            st->sample_count = count;
            if (const_size == 0) {
                st->sizes = (uint32_t *)malloc(sizeof(uint32_t) * count);
                for (uint32_t i = 0; i < count; i++)
                    st->sizes[i] = rd_u32(box_body + 12 + i * 4);
            }
        } else if (strcmp(type, "stco") == 0) {
            uint32_t count = rd_u32(box_body + 4);
            st->chunk_offsets = (uint64_t *)malloc(sizeof(uint64_t) * count);
            st->chunk_count = count;
            for (uint32_t i = 0; i < count; i++)
                st->chunk_offsets[i] = rd_u32(box_body + 8 + i * 4);
        } else if (strcmp(type, "co64") == 0) {
            uint32_t count = rd_u32(box_body + 4);
            st->chunk_offsets = (uint64_t *)malloc(sizeof(uint64_t) * count);
            st->chunk_count = count;
            for (uint32_t i = 0; i < count; i++)
                st->chunk_offsets[i] = rd_u64(box_body + 8 + i * 8);
        } else if (strcmp(type, "stsc") == 0) {
            uint32_t count = rd_u32(box_body + 4);
            st->stsc_first_chunk = (uint32_t *)malloc(sizeof(uint32_t) * count);
            st->stsc_spc = (uint32_t *)malloc(sizeof(uint32_t) * count);
            st->stsc_count = count;
            for (uint32_t i = 0; i < count; i++) {
                st->stsc_first_chunk[i] = rd_u32(box_body + 8 + i * 12);
                st->stsc_spc[i] = rd_u32(box_body + 12 + i * 12);
            }
        }

        p = box_start + box_total;
    }
}

static void walk_boxes(ParseState *st, unsigned char *p, unsigned char *end) {
    while (p + 8 <= end) {
        uint32_t size = rd_u32(p);
        char type[5] = {0};
        memcpy(type, p + 4, 4);
        unsigned char *box_start = p;
        unsigned char *box_body = p + 8;
        uint32_t box_total = size;
        if (size == 1) {
            box_total = (uint32_t)rd_u64(p + 8);
            box_body = p + 16;
        }
        if (size == 0) box_total = (uint32_t)(end - box_start);
        if (box_start + box_total > end || box_total < 8) break;

        if (strcmp(type, "moov") == 0 || strcmp(type, "trak") == 0 ||
            strcmp(type, "mdia") == 0 || strcmp(type, "minf") == 0) {
            walk_boxes(st, box_body, box_start + box_total);
        } else if (strcmp(type, "stbl") == 0) {
            walk_stbl(st, box_body, box_start + box_total);
        }

        p = box_start + box_total;
    }
}

int mp4_open(const char *path, Mp4Movie *mov) {
    memset(mov, 0, sizeof(*mov));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *data = (unsigned char *)malloc((size_t)size);
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(data);
        return -1;
    }
    fclose(f);

    ParseState st;
    memset(&st, 0, sizeof(st));
    walk_boxes(&st, data, data + size);

    if (!st.found_video_track || st.sample_count == 0 || st.chunk_count == 0 || st.stsc_count == 0) {
        free(data);
        free(st.sizes);
        free(st.chunk_offsets);
        free(st.stsc_first_chunk);
        free(st.stsc_spc);
        return -1;
    }

    mov->file_data = data;
    mov->file_size = size;
    mov->sps = st.sps;
    mov->sps_len = st.sps_len;
    mov->pps = st.pps;
    mov->pps_len = st.pps_len;
    mov->nal_length_size = st.nal_length_size;
    mov->width = st.width;
    mov->height = st.height;
    mov->found_video_track = 1;

    mov->samples = (Mp4Sample *)malloc(sizeof(Mp4Sample) * st.sample_count);
    mov->sample_count = st.sample_count;

    uint32_t sample_idx = 0;
    for (uint32_t chunk = 1; chunk <= st.chunk_count && sample_idx < st.sample_count; chunk++) {
        uint32_t spc = st.stsc_spc[st.stsc_count - 1];
        for (uint32_t k = 0; k < st.stsc_count; k++) {
            if (st.stsc_first_chunk[k] <= chunk &&
                (k + 1 == st.stsc_count || st.stsc_first_chunk[k + 1] > chunk)) {
                spc = st.stsc_spc[k];
                break;
            }
        }
        uint64_t offset = st.chunk_offsets[chunk - 1];
        for (uint32_t s = 0; s < spc && sample_idx < st.sample_count; s++, sample_idx++) {
            uint32_t samp_size = st.const_size ? st.const_size : st.sizes[sample_idx];
            mov->samples[sample_idx].offset = offset;
            mov->samples[sample_idx].size = samp_size;
            offset += samp_size;
        }
    }

    free(st.sizes);
    free(st.chunk_offsets);
    free(st.stsc_first_chunk);
    free(st.stsc_spc);
    return 0;
}

void mp4_free(Mp4Movie *mov) {
    free(mov->file_data);
    free(mov->sps);
    free(mov->pps);
    free(mov->samples);
    memset(mov, 0, sizeof(*mov));
}

unsigned char *mp4_build_avcc(const Mp4Movie *mov, int *out_len) {
    /* configurationVersion(1)=1 profile(1) compat(1) level(1)
     * lengthSizeMinusOne|0xfc(1) numSPS|0xe0(1) [len(2) data]...
     * numPPS(1) [len(2) data]... */
    int len = 6 + 2 + mov->sps_len + 1 + 2 + mov->pps_len;
    unsigned char *b = (unsigned char *)malloc((size_t)len);
    int i = 0;
    b[i++] = 1;
    b[i++] = mov->sps[1];
    b[i++] = mov->sps[2];
    b[i++] = mov->sps[3];
    b[i++] = (unsigned char)(0xfc | (mov->nal_length_size - 1));
    b[i++] = (unsigned char)(0xe0 | 1);
    b[i++] = (unsigned char)((mov->sps_len >> 8) & 0xff);
    b[i++] = (unsigned char)(mov->sps_len & 0xff);
    memcpy(b + i, mov->sps, (size_t)mov->sps_len);
    i += mov->sps_len;
    b[i++] = 1;
    b[i++] = (unsigned char)((mov->pps_len >> 8) & 0xff);
    b[i++] = (unsigned char)(mov->pps_len & 0xff);
    memcpy(b + i, mov->pps, (size_t)mov->pps_len);
    i += mov->pps_len;
    *out_len = i;
    return b;
}
