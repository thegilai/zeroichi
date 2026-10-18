/* Zero Ichi - animated GIFs: writing (LZW, one shared palette) and reading */
#ifndef ZEROICHI_GIF_H
#define ZEROICHI_GIF_H
#include <stdint.h>
#include "img.h"

/* frames: grey images holding colour numbers into pal (ncolors entries) */
int gif_write(const char *path, Img *frames, int n, const uint8_t (*pal)[3], int ncolors, int delay_ms,
              const char *look);      /* look: in the comment (NULL - none) */
/* one shared palette (<= maxcol colours) for R,G,B frames; idx[i] = colour numbers */
int gif_quantize(Img *rgb, int n, int maxcol, uint8_t (*pal)[3], Img *idx);

typedef struct GifReader GifReader;
GifReader *gifr_open(const char *path);
int gifr_size(GifReader *g, int *w, int *h);
/* the next frame, as the whole picture (R,G,B, owned by the reader): 1, or 0 at the end */
int gifr_next(GifReader *g, Img *rgb, double *seconds);
void gifr_close(GifReader *g);
#endif
