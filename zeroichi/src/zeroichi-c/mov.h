/* Zero Ichi - QuickTime (.mov) Photo-JPEG videos: each frame is a JPEG and keeps
 * its real time stamp (a port of movie.py; plays in QuickTime and VLC). */
#ifndef ZEROICHI_MOV_H
#define ZEROICHI_MOV_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    int w, h, rotation;
    uint64_t pos, mdat_pos;
    volatile uint32_t kb;            /* KiB written so far (read by the recording loop) */
    uint32_t *sizes, *offsets;
    double *times;
    int n, cap;
    char look[48];                   /* the look: the description "Look: ..." ("": none) */
} MovWriter;

int mov_open(MovWriter *m, const char *path, int w, int h, int rotation, const char *look);
int mov_add(MovWriter *m, const uint8_t *jpeg, size_t len, double t);   /* adds Huffman tables if needed */
double mov_duration(const MovWriter *m);
int mov_close(MovWriter *m);             /* writes the index, fsync */

typedef struct {
    FILE *f;
    int n, w, h, rotation;
    uint32_t *sizes, *offsets;
    double *durations;
} MovReader;

int movr_open(MovReader *r, const char *path);
uint8_t *movr_frame(MovReader *r, int i, size_t *len);    /* malloc'd JPEG */
double movr_duration(const MovReader *r);
void movr_close(MovReader *r);
#endif
