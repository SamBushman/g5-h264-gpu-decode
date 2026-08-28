#ifndef MP4BOX_H
#define MP4BOX_H

#include <stdint.h>

typedef struct {
    uint64_t offset;
    uint32_t size;
} Mp4Sample;

typedef struct {
    unsigned char *file_data;
    long file_size;

    unsigned char *sps;
    int sps_len;
    unsigned char *pps;
    int pps_len;
    int nal_length_size;

    int width, height;
    int found_video_track;

    Mp4Sample *samples;
    uint32_t sample_count;
} Mp4Movie;

/* Loads path, parses the moov box tree, and fills out *mov (which owns
 * mov->file_data - call mp4_free when done). Returns 0 on success. */
int mp4_open(const char *path, Mp4Movie *mov);
void mp4_free(Mp4Movie *mov);

/* Builds an avcC box (as used for AVCodecContext.extradata) from the
 * parsed sps/pps. Caller frees the returned buffer. */
unsigned char *mp4_build_avcc(const Mp4Movie *mov, int *out_len);

#endif
