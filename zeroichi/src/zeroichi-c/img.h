/* Zero Ichi - 8-bit images (grey "L" or R,G,B) and the picture operations the
 * looks are made of. The resampling, reducing, filtering and channel maths
 * follow Pillow's own C code (libImaging), so the looks come out the same
 * as in the Python version of Zero Ichi. */
#ifndef ZEROICHI_IMG_H
#define ZEROICHI_IMG_H
#include <stdint.h>

typedef struct {
    int w, h, c;        /* c = 1 (grey) or 3 (R,G,B) */
    int stride;         /* bytes per row */
    uint8_t *p;
    int own;            /* 1: p was allocated for this image */
} Img;

#define PX(im, x, y) ((im).p + (size_t)(y) * (im).stride + (size_t)(x) * (im).c)

Img img_new(int w, int h, int c);             /* zero-filled */
Img img_alloc(int w, int h, int c);          /* like img_new, not cleared (every pixel gets written) */
Img img_view(uint8_t *p, int w, int h, int c, int stride);
Img img_sub(Img a, int x, int y, int w, int h);   /* a view of part of a */
Img img_copy(Img a);
void img_free(Img *a);
void img_fill(Img a, int r, int g, int b);

enum { RS_NEAREST = 0, RS_BOX = 4, RS_BILINEAR = 2 };

/* Image.resize((w, h), filter) */
Img img_resize(Img a, int w, int h, int filter);
void img_resize_into(Img a, Img dst, int filter);    /* the same, into dst (its size) */
/* enlarging to w x h (bilinear, the same numbers as img_resize) one row at a
 * time, for a pass that uses each row once: NULL when it isn't that kind of
 * enlargement (then img_resize). A row stays valid until the next call. */
typedef struct ImgRows ImgRows;
ImgRows *img_rows_begin(Img a, int w, int h);
const uint8_t *img_rows_get(ImgRows *e, int y);
void img_rows_end(ImgRows *e);
/* Image.resize(..., reducing_gap=gap) */
Img img_resize_gap(Img a, int w, int h, int filter, double gap);
/* Image.reduce(f) and reduce((fx, fy)) */
Img img_reduce(Img a, int f);
Img img_reduce2(Img a, int fx, int fy);
/* filters._shrink_fast: reduce() by whole blocks, then a bilinear resize */
Img img_shrink_fast(Img a, int w, int h);

/* point(): one table for every channel, or one per channel */
void img_point(Img a, const uint8_t *lut);
void img_point3(Img a, const uint8_t *lr, const uint8_t *lg, const uint8_t *lb);
/* ImageChops */
void img_multiply(Img a, Img b);              /* a = a * b / 255  (b grey or same) */
void img_add_modulo(Img a, Img b);            /* a = (a + b) & 255 */
void img_add(Img a, Img b, float scale, int offset);
Img img_offset(Img a, int dx, int dy);        /* wraps around */
/* Image.composite(a, b, mask) -> written into a */
void img_composite(Img a, Img b, Img mask);
/* ImageFilter.Kernel((3, 3), k, scale) on a grey image */
Img img_filter3x3(Img a, const float k[9], float scale, float offset);

/* colour conversions (Pillow's) */
Img img_to_l(Img a);                          /* convert("L") */
Img img_to_rgb(Img a);                        /* grey -> R,G,B */
void img_rgb_to_ycc(Img rgb, Img *y, Img *cb, Img *cr);      /* full size planes */
void img_rgb_to_ycc_into(Img rgb, Img y, Img cb, Img cr);   /* (planes of rgb's size) */
void img_rgb_to_yuv420(Img rgb, Img *y, Img *cb, Img *cr);   /* Cb, Cr: then reduce(2) */
void img_rgb_to_yuv420_into(Img rgb, Img y, Img cb, Img cr);   /* (even sizes) */
Img img_ycc_to_rgb(Img y, Img cb, Img cr);    /* all the same size */
void ycc_px_to_rgb(int y, int cb, int cr, uint8_t *out);
void rgb_px_to_ycc(int r, int g, int b, uint8_t *out);

/* drawing (ImageDraw) on R,G,B images */
void img_rect(Img a, int x0, int y0, int x1, int y1, int r, int g, int b);   /* inclusive */
void img_vline(Img a, int x, int y0, int y1, int width, int r, int g, int b);

/* turning pictures */
/* the live view from the camera's planes: turned (90 degrees clockwise) if
 * asked, nearest-resized to tw x th - R,G,B (U, V at half size), or one plane */
Img img_view_rgb(Img Y, Img U, Img V, int turn, int tw, int th);
Img img_view_plane(Img a, int turn, int tw, int th);
void img_nearest_map(int sw, int dw, int *xs);
Img img_rot90cw(Img a);                       /* 90 degrees clockwise */
Img img_rot90ccw(Img a);
Img img_rot180(Img a);
Img img_flip(Img a, int hflip, int vflip);

#endif
