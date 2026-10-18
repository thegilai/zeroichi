/* Zero Ichi - the screen saver (the Bauhaus "W" design: shapes drifting and
 * turning slowly behind a sleeping "0 1") and the start-up picture drawn by
 * the program itself (when splash*.rgb565 is missing) */
#ifndef ZEROICHI_SCREENSAVER_H
#define ZEROICHI_SCREENSAVER_H
#include "canvas.h"

#define SAVER_FPS 10.0          /* pictures a second (each about 2-3 ms of work on a Pi Zero) */
#define SAVER_SPEED 3.0         /* the design's time units a second (how quickly the shapes move) */

/* the screen saver at `seconds` after it began, into the canvas */
void saver_draw(Canvas *c, double seconds);
void saver_free(void);          /* (its working picture: until the next time) */
/* the start-up picture (the design "S"), with "v51 . 320x240" */
void splash_draw(Canvas *c, const char *version);

/* (v52) Zero Ichi's colours and the menus' shapes (smooth edges, on the
 * screen's 16-bit picture) */
static const Rgb ZI_CREAM = {242, 234, 216}, ZI_PAPER = {232, 222, 200}, ZI_RED = {220, 58, 40},
                 ZI_YEL = {246, 184, 26}, ZI_BLU = {28, 76, 156}, ZI_INK = {24, 24, 26},
                 ZI_GREY = {120, 112, 98};
void ui_rect(Canvas *c, float x0, float y0, float x1, float y1, Rgb col);   /* (edges: x1, y1 not included) */
void ui_circle(Canvas *c, float cx, float cy, float r, Rgb col);
void ui_poly(Canvas *c, const float *xy, int n, Rgb col);                    /* convex, up to 8 corners */
void ui_quarter(Canvas *c, float cx, float cy, float r, float a, Rgb col);   /* a .. a + 90 degrees */
void ui_mark(Canvas *c, float cx, float cy, float r, Rgb bg);                /* the little "0 1" */
/* (v53) the whole mark, as on the start-up picture (Zero peeking): its ring's
 * middle at (x, y), s = its size (1: the ring's radius is 60) */
void ui_logo(Canvas *c, float x, float y, float s, Rgb bg);
float ui_logo_width(float s);                                                /* (from x - 60 s) */
/* (v53) a picture with its own transparency (brandart.c), top left at (x, y) */
struct BrandArt;
void ui_art(Canvas *c, const struct BrandArt *a, int x, int y);
#endif
