/* Zero Ichi - JPEG: software (libjpeg-turbo) and the Pi's hardware encoder */
#ifndef ZEROICHI_JPEG_H
#define ZEROICHI_JPEG_H
#include <stddef.h>
#include <stdint.h>
#include "img.h"
#include "util.h"

/* software */
int jpeg_encode_yuv(const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v, int cstride,
                    int w, int h, int quality, Buf *out);
int jpeg_encode_rgb(Img rgb, int quality, Buf *out);
/* decode, at a reduced size when allowed: the result is at least min_w x min_h
 * (0 = full size). mode: 3 = R,G,B, 1 = grey, 2 = Y/Cb/Cr planes (in *y,*cb,*cr) */
int jpeg_size(const uint8_t *data, size_t n, int *w, int *h);
Img jpeg_decode(const uint8_t *data, size_t n, int min_w, int min_h, int gray);
int jpeg_decode_ycc(const uint8_t *data, size_t n, int min_w, int min_h, Img *y, Img *cb, Img *cr);

/* frames from the hardware MJPEG encoder leave out the standard Huffman tables */
int jpeg_needs_huffman(const uint8_t *d, size_t n);
void jpeg_add_huffman(const uint8_t *d, size_t n, Buf *out);
void jpeg_add_exif(const uint8_t *d, size_t n, const char *look, Buf *out);   /* look: NULL - none */

/* the hardware encoder, for one size and input format (tested with a test
 * picture the first time); NULL if it can't be used */
typedef struct HwJpeg HwJpeg;
HwJpeg *hwjpeg_open(int w, int h, int yuv, double bits);
int hwjpeg_encode_yuv(HwJpeg *e, const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v,
                      int cstride, Buf *out);
int hwjpeg_encode_rgb(HwJpeg *e, Img rgb, Buf *out);
void hwjpeg_close(HwJpeg *e);
/* the sizes it was checked for: remembered in `path` from one start to the
 * next (hwjpeg_save writes them when hwjpeg_unsaved says there's news) */
void hwjpeg_remember(const char *path);
int hwjpeg_unsaved(void);
int hwjpeg_save(void);

/* JpegMaker: hardware when it works (each size tested once), else software */
typedef struct {
    int w, h, quality;
    double bits;
    HwJpeg *hw[2];                /* [0] R,G,B  [1] YUV */
    int tried[2];
    int used_hw, used_sw;
    uint8_t *sw;                  /* (jm_planes without the hardware: its own buffer) */
    int planes_hw;
} JpegMaker;
void jm_init(JpegMaker *m, int w, int h, int quality, double bits);
int jm_yuv(JpegMaker *m, const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v, int cstride,
           Buf *out);
int jm_rgb(JpegMaker *m, Img rgb, Buf *out);
/* a YUV frame made in place: jm_planes gives the planes to fill (the hardware
 * encoder's own buffer - nothing copied), jm_encode_planes compresses them */
int jm_planes(JpegMaker *m, uint8_t **y, int *ystride, uint8_t **u, uint8_t **v, int *cstride);
int jm_encode_planes(JpegMaker *m, Buf *out);
void hwjpeg_planes(HwJpeg *e, uint8_t **y, int *ystride, uint8_t **u, uint8_t **v, int *cstride);
const char *jm_how(JpegMaker *m);
void jm_close(JpegMaker *m);
#endif
