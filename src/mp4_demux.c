/*
 * mp4-demux: Milestone 2 of the G5/X1900 H.264 GPU-decode test program.
 *
 * Minimal ISO-BMFF (MP4) box parser. Walks ftyp/moov/trak/mdia/minf/stbl to
 * find the video track's avcC (SPS/PPS) and sample table (stsz/stco/stsc),
 * then dumps each sample's NAL units (as found in mdat, AVCC length-prefixed).
 *
 * No third-party deps - just this file. Big-endian box fields are read
 * explicitly (not via host-endianness struct overlays) so this stays
 * correct regardless of host byte order, even though PPC's big-endian
 * layout happens to match MP4's on-disk format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct {
    unsigned char *data;
    long size;
} Buf;

static Buf read_file(const char *path) {
    Buf b = {0, 0};
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    fseek(f, 0, SEEK_END);
    b.size = ftell(f);
    fseek(f, 0, SEEK_SET);
    b.data = (unsigned char *)malloc((size_t)b.size);
    if (fread(b.data, 1, (size_t)b.size, f) != (size_t)b.size) {
        fprintf(stderr, "short read on %s\n", path);
        exit(1);
    }
    fclose(f);
    return b;
}

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

/* ---- sample table state, filled in while walking stbl ---- */

typedef struct {
    uint32_t *sizes;
    uint32_t sample_count;
    uint32_t const_size; /* stsz: nonzero if all samples are this size */

    uint64_t *chunk_offsets;
    uint32_t chunk_count;

    /* stsc entries: first_chunk, samples_per_chunk, sample_desc_index */
    uint32_t *stsc_first_chunk;
    uint32_t *stsc_spc;
    uint32_t stsc_count;

    unsigned char *sps;
    int sps_len;
    unsigned char *pps;
    int pps_len;
    int nal_length_size; /* from avcC: 1, 2, or 4 */

    int width, height;
    int found_video_track;
} MovState;

static void parse_avcC(MovState *st, const unsigned char *box, uint32_t box_size) {
    /* avcC: configurationVersion(1) AVCProfileIndication(1) profile_compat(1)
     * AVCLevelIndication(1) 6bits-reserved+2bits-lengthSizeMinusOne(1)
     * 3bits-reserved+5bits-numSPS(1) then per-SPS: len(2)+data
     * then numPPS(1) then per-PPS: len(2)+data
     */
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

/* Minimal SPS bit reader, just enough for profile/level/width/height sanity
 * checking (not a full exp-golomb SPS parser - that lives in Module C via
 * FFmpeg, not here). */
static void print_sps_summary(const unsigned char *sps, int len) {
    if (len < 4) return;
    int profile_idc = sps[1];
    int level_idc = sps[3];
    printf("  SPS: profile_idc=%d level_idc=%d (%d bytes)\n", profile_idc, level_idc, len);
}

/* ---- box walking ---- */

static void walk_boxes(MovState *st, unsigned char *p, unsigned char *end, int depth, const char *path_ctx);

static void walk_stbl(MovState *st, unsigned char *p, unsigned char *end) {
    while (p + 8 <= end) {
        uint32_t size = rd_u32(p);
        char type[5] = {0};
        memcpy(type, p + 4, 4);
        unsigned char *box_start = p;
        unsigned char *box_body = p + 8;
        uint32_t box_total = size;
        if (size == 1) {
            /* 64-bit extended size, rare for these small boxes; handle anyway */
            box_total = (uint32_t)rd_u64(p + 8);
            box_body = p + 16;
        }
        if (size == 0 || box_start + box_total > end) break;

        if (strcmp(type, "stsd") == 0) {
            /* stsd: version/flags(4) entry_count(4) then entries; first
             * entry for video is avc1, which itself contains an avcC box */
            unsigned char *q = box_body + 8; /* skip version/flags + entry_count */
            if (q + 8 <= box_start + box_total) {
                uint32_t entry_size = rd_u32(q);
                char entry_type[5] = {0};
                memcpy(entry_type, q + 4, 4);
                if (strcmp(entry_type, "avc1") == 0 && q + entry_size <= box_start + box_total) {
                    /* avc1 sample entry: 6 bytes reserved, 2 data_ref_index,
                     * 16 bytes pre-defined/reserved, width(2) height(2), ... */
                    unsigned char *e = q + 8;
                    if (e + 32 <= q + entry_size) {
                        st->width = rd_u16(e + 24);
                        st->height = rd_u16(e + 26);
                    }
                    /* Find avcC nested inside this sample entry */
                    unsigned char *inner = q + 8 + 78; /* fixed sample-entry header size */
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

static void walk_boxes(MovState *st, unsigned char *p, unsigned char *end, int depth, const char *path_ctx) {
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
        if (size == 0) {
            /* box extends to end of parent (rare, usually only top-level mdat) */
            box_total = (uint32_t)(end - box_start);
        }
        if (box_start + box_total > end || box_total < 8) break;

        for (int i = 0; i < depth; i++) printf("  ");
        printf("%s [%s] size=%u\n", path_ctx, type, box_total);

        if (strcmp(type, "moov") == 0 || strcmp(type, "trak") == 0 ||
            strcmp(type, "mdia") == 0 || strcmp(type, "minf") == 0) {
            walk_boxes(st, box_body, box_start + box_total, depth + 1, type);
        } else if (strcmp(type, "stbl") == 0) {
            walk_stbl(st, box_body, box_start + box_total);
        }

        p = box_start + box_total;
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <file.mp4> [--dump-nals]\n", argv[0]);
        return 1;
    }
    int dump_nals = (argc > 2 && strcmp(argv[2], "--dump-nals") == 0);

    Buf f = read_file(argv[1]);
    printf("file: %s (%ld bytes)\n", argv[1], f.size);

    MovState st;
    memset(&st, 0, sizeof(st));

    walk_boxes(&st, f.data, f.data + f.size, 0, "top");

    printf("\n--- summary ---\n");
    if (!st.found_video_track) {
        printf("no H.264 (avc1) video track found\n");
        return 1;
    }
    printf("video: %dx%d, NAL length size=%d\n", st.width, st.height, st.nal_length_size);
    if (st.sps) print_sps_summary(st.sps, st.sps_len);
    if (st.pps) printf("  PPS: %d bytes\n", st.pps_len);
    printf("sample_count=%u chunk_count=%u stsc_count=%u\n",
           st.sample_count, st.chunk_count, st.stsc_count);
    for (uint32_t i = 0; i < st.stsc_count; i++)
        printf("  stsc[%u]: first_chunk=%u samples_per_chunk=%u\n",
               i, st.stsc_first_chunk[i], st.stsc_spc[i]);
    for (uint32_t i = 0; i < (st.chunk_count < 3 ? st.chunk_count : 3); i++)
        printf("  chunk_offsets[%u]=%llu\n", i, (unsigned long long)st.chunk_offsets[i]);

    if (st.sample_count == 0 || st.chunk_count == 0 || st.stsc_count == 0) {
        printf("incomplete sample tables, cannot walk samples\n");
        return 1;
    }

    /* Expand stsc into per-chunk sample counts, then walk chunks/samples
     * to print (and optionally dump) each sample's NAL units. */
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
            if (sample_idx < 5 || sample_idx >= st.sample_count - 2) {
                printf("sample %u: offset=%llu size=%u\n", sample_idx,
                       (unsigned long long)offset, samp_size);
            }
            if (dump_nals && offset + samp_size <= (uint64_t)f.size) {
                unsigned char *sp = f.data + offset;
                uint32_t remaining = samp_size;
                while (remaining > (uint32_t)st.nal_length_size) {
                    uint32_t nal_len = 0;
                    for (int b = 0; b < st.nal_length_size; b++)
                        nal_len = (nal_len << 8) | sp[b];
                    sp += st.nal_length_size;
                    remaining -= st.nal_length_size;
                    if (nal_len == 0 || nal_len > remaining) break;
                    int nal_type = sp[0] & 0x1f;
                    if (sample_idx < 5)
                        printf("    NAL: len=%u type=%d\n", nal_len, nal_type);
                    sp += nal_len;
                    remaining -= nal_len;
                }
            }
            offset += samp_size;
        }
    }

    return 0;
}
