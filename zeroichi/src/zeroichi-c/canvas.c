#include "canvas.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"

Canvas cv_new(int w, int h) {
    Canvas c = {w, h, calloc((size_t)w * h, 2)};
    return c;
}

void cv_free(Canvas *c) {
    free(c->px);
    c->px = NULL;
}

void cv_copy(Canvas *dst, const Canvas *src) {
    memcpy(dst->px, src->px, (size_t)src->w * src->h * 2);
}

void cv_fill(Canvas *c, Rgb col) {
    uint16_t v = rgb565c(col);
    for (int i = 0; i < c->w * c->h; i++) c->px[i] = v;
}

void cv_rect(Canvas *c, int x0, int y0, int x1, int y1, Rgb col) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (y1 >= c->h) y1 = c->h - 1;
    uint16_t v = rgb565c(col);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) c->px[y * c->w + x] = v;
}

void cv_line_h(Canvas *c, int x0, int x1, int y, Rgb col) { cv_rect(c, x0, y, x1, y, col); }
void cv_line_v(Canvas *c, int x, int y0, int y1, Rgb col) { cv_rect(c, x, y0, x, y1, col); }

void cv_rect_outline(Canvas *c, int x0, int y0, int x1, int y1, Rgb col) {
    cv_line_h(c, x0, x1, y0, col);
    cv_line_h(c, x0, x1, y1, col);
    cv_line_v(c, x0, y0, y1, col);
    cv_line_v(c, x1, y0, y1, col);
}

static inline uint16_t dim565(uint16_t v) {      /* 1/4 + 1/16 of each channel */
    return (uint16_t)(((v >> 2) & 0x39E7) + ((v >> 4) & 0x0861));
}

void cv_dim(Canvas *c, int x0, int y0, int x1, int y1) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= c->w) x1 = c->w - 1;
    if (y1 >= c->h) y1 = c->h - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) c->px[y * c->w + x] = dim565(c->px[y * c->w + x]);
}

/* how far a row of a rounded rectangle is pulled in at the corners */
static int corner_inset(int y, int y0, int y1, int r) {
    int dy = -1;
    if (y < y0 + r) dy = y0 + r - y;
    else if (y > y1 - r) dy = y - (y1 - r);
    if (dy < 0) return 0;
    double dx = sqrt((double)r * r - (double)(dy - 0.5) * (dy - 0.5));
    return r - (int)(dx + 0.5);
}

void cv_round_rect(Canvas *c, int x0, int y0, int x1, int y1, int r, int fill, Rgb fcol,
                   int dim, int outline, Rgb ocol) {
    uint16_t fv = rgb565c(fcol), ov = rgb565c(ocol);
    for (int y = y0; y <= y1; y++) {
        if (y < 0 || y >= c->h) continue;
        int in = corner_inset(y, y0, y1, r);
        int a = x0 + in, b = x1 - in;
        int prev = y > y0 ? corner_inset(y - 1, y0, y1, r) : 99999;
        int next = y < y1 ? corner_inset(y + 1, y0, y1, r) : 99999;
        for (int x = a; x <= b; x++) {
            if (x < 0 || x >= c->w) continue;
            uint16_t *p = &c->px[y * c->w + x];
            int edge = y == y0 || y == y1 || x == a || x == b ||
                       x < x0 + (prev < in ? in : prev) || x > x1 - (prev < in ? in : prev) ||
                       x < x0 + (next < in ? in : next) || x > x1 - (next < in ? in : next);
            if (outline && edge) *p = ov;
            else if (fill) *p = fv;
            else if (dim) *p = dim565(*p);
        }
    }
}

void cv_circle(Canvas *c, int cx, int cy, int r, Rgb col) {
    uint16_t v = rgb565c(col);
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && py >= 0 && px < c->w && py < c->h) c->px[py * c->w + px] = v;
            }
}

static inline void blend(uint16_t *p, Rgb col, int a) {
    if (a <= 0) return;
    if (a >= 255) {
        *p = rgb565c(col);
        return;
    }
    uint16_t v = *p;
    int r = (v >> 11) << 3, g = ((v >> 5) & 63) << 2, b = (v & 31) << 3;
    r += (col.r - r) * a / 255;
    g += (col.g - g) * a / 255;
    b += (col.b - b) * a / 255;
    *p = C565(r, g, b);
}

static void draw_mask(Canvas *c, const unsigned char *base, const GlyphMask *m, int px, int py, Rgb col) {
    const unsigned char *bits = base + m->off;
    for (int y = 0; y < m->h; y++) {
        int yy = py + m->y + y;
        if (yy < 0 || yy >= c->h) continue;
        for (int x = 0; x < m->w; x++) {
            int xx = px + m->x + x;
            if (xx < 0 || xx >= c->w) continue;
            blend(&c->px[yy * c->w + xx], col, bits[y * m->w + x]);
        }
    }
}

double cv_text_width(int size, const char *s) { return text_width(font(size), s); }

static void text_f(Canvas *c, const Font *f, double x, double y, const char *s, Rgb col, const char *anchor,
                   int stroke);

void cv_text(Canvas *c, int size, double x, double y, const char *s, Rgb col,
             const char *anchor, int stroke) {
    text_f(c, font(size), x, y, s, col, anchor, stroke);
}

void cv_text_f(Canvas *c, const Font *f, double x, double y, const char *s, Rgb col, const char *anchor) {
    text_f(c, f, x, y, s, col, anchor, 0);
}

static void text_f(Canvas *c, const Font *f, double x, double y, const char *s, Rgb col, const char *anchor,
                   int stroke) {
    if (!anchor) anchor = "mm";
    double w = text_width(f, s);
    double x0 = anchor[0] == 'm' ? x - w / 2 : anchor[0] == 'r' ? x - w : x;
    double base = y;
    switch (anchor[1]) {
    case 'a': base = y + f->ascent; break;
    case 'm': base = y + (f->ascent - f->descent) / 2.0; break;
    case 'd': base = y - f->descent; break;
    default: break;
    }
    int by = (int)floor(base + 0.5);
    for (int pass = stroke ? 0 : 1; pass < 2; pass++) {
        const char *p = s;
        int64_t pen = 0;
        while (*p) {
            const Glyph *g = font_glyph(f, utf8_next(&p));
            if (!g) continue;
            int gx = (int)floor(x0 + pen / 64.0 + 0.5);
            if (pass == 0)
                draw_mask(c, f->bits ? f->bits : FONT_BITS, &g->stroke, gx, by, BLACK);
            else
                draw_mask(c, f->bits ? f->bits : FONT_BITS, &g->fill, gx, by, col);
            pen += g->adv64;
        }
    }
}

void cv_blit_rgb(Canvas *c, Img a, int x, int y, int w, int h) {
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= c->h) continue;
        int sy = (int)((j + 0.5) * a.h / h);
        const uint8_t *row = PX(a, 0, sy);
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= c->w) continue;
            const uint8_t *p = row + (int)((i + 0.5) * a.w / w) * a.c;
            c->px[yy * c->w + xx] = a.c == 3 ? C565(p[0], p[1], p[2]) : C565(p[0], p[0], p[0]);
        }
    }
}

void cv_blit_565(Canvas *c, const uint16_t *src, int sw, int sh, int x, int y, int w, int h) {
    if (w == sw && h == sh) {
        for (int j = 0; j < h; j++) {
            int yy = y + j;
            if (yy < 0 || yy >= c->h) continue;
            if (x >= 0 && x + w <= c->w) {
                memcpy(&c->px[yy * c->w + x], src + j * sw, (size_t)w * 2);
                continue;
            }
            for (int i = 0; i < w; i++)
                if (x + i >= 0 && x + i < c->w) c->px[yy * c->w + x + i] = src[j * sw + i];
        }
        return;
    }
    int *xs = malloc(sizeof(int) * w);
    for (int i = 0; i < w; i++) xs[i] = (int)((i + 0.5) * sw / w);
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= c->h) continue;
        const uint16_t *row = src + (int)((j + 0.5) * sh / h) * sw;
        uint16_t *o = &c->px[yy * c->w];
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx >= 0 && xx < c->w) o[xx] = row[xs[i]];
        }
    }
    free(xs);
}
