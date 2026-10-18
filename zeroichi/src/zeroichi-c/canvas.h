/* Zero Ichi - drawing on the screen picture (16-bit RGB565, native byte order) */
#ifndef ZEROICHI_CANVAS_H
#define ZEROICHI_CANVAS_H
#include <stdint.h>
#include "font.h"
#include "img.h"

typedef struct { int w, h; uint16_t *px; } Canvas;
typedef struct { uint8_t r, g, b; } Rgb;

#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
static inline uint16_t rgb565c(Rgb c) { return C565(c.r, c.g, c.b); }
static const Rgb WHITE = {255, 255, 255}, BLACK = {0, 0, 0}, YELLOW = {255, 220, 80};

Canvas cv_new(int w, int h);
void cv_free(Canvas *c);
void cv_fill(Canvas *c, Rgb col);
void cv_rect(Canvas *c, int x0, int y0, int x1, int y1, Rgb col);          /* inclusive */
void cv_rect_outline(Canvas *c, int x0, int y0, int x1, int y1, Rgb col);
void cv_round_rect(Canvas *c, int x0, int y0, int x1, int y1, int r, int fill, Rgb fcol,
                   int dim, int outline, Rgb ocol);
void cv_dim(Canvas *c, int x0, int y0, int x1, int y1);                   /* ~1/3 brightness */
void cv_line_h(Canvas *c, int x0, int x1, int y, Rgb col);
void cv_line_v(Canvas *c, int x, int y0, int y1, Rgb col);
void cv_circle(Canvas *c, int cx, int cy, int r, Rgb col);
/* text: anchor like Pillow's ("mm", "lm", "rm", "ma", "ra", "la", "ls"...);
 * stroke = black outline (2 px, as Zero Ichi's text()) */
void cv_text(Canvas *c, int size, double x, double y, const char *s, Rgb col,
             const char *anchor, int stroke);
double cv_text_width(int size, const char *s);
/* (v52) text in a given font (e.g. brand_font: the menus' Poppins), no outline */
void cv_text_f(Canvas *c, const Font *f, double x, double y, const char *s, Rgb col, const char *anchor);
/* pictures */
void cv_blit_rgb(Canvas *c, Img rgb, int x, int y, int w, int h);          /* scaled (nearest) */
void cv_blit_565(Canvas *c, const uint16_t *src, int sw, int sh, int x, int y, int w, int h);
void cv_copy(Canvas *dst, const Canvas *src);
#endif
