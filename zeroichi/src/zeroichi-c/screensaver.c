/* Zero Ichi - the screen saver and the start-up picture's fallback (see
 * screensaver.h). The shapes are drawn with smooth edges straight from their
 * geometry (each shape is convex: a circle and/or straight edges), into an
 * R,G,B picture that becomes the screen's 16-bit picture at the end. The
 * sleeping "0 1" and the text are pictures (brandart.c). Python: app.py,
 * SaverArt - the same design. */
#include "screensaver.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "brandart.h"
#include "img.h"

static const uint8_t CREAM[3] = {242, 234, 216}, RED[3] = {220, 58, 40},
                     YEL[3] = {246, 184, 26}, BLU[3] = {28, 76, 156}, BLK[3] = {24, 24, 26};

/* ------------------------------------------------------------ drawing */
typedef struct { float nx, ny, d; } Edge;       /* inside: nx*x + ny*y + d >= 0 (|n| = 1) */

/* where shapes are drawn: an R,G,B picture, or (c set) the screen's own
 * 16-bit picture - the menus (ui_*) */
typedef struct { Img im; Canvas *c; } Surf;

static inline void put(uint8_t *q, const uint8_t *c, int a) {     /* a: 0..256 */
    if (a >= 256) {
        q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
        return;
    }
    for (int k = 0; k < 3; k++) q[k] = (uint8_t)(q[k] + (((int)c[k] - q[k]) * a >> 8));
}

/* (rounded to the nearest 16-bit colour, not cut: 23 and 24 - the same dark
 * background, one drawn, one from a picture - are then the same colour) */
static uint8_t R5[256], G6[256];

static void r5_init(void) {
    if (!R5[255])
        for (int v = 0; v < 256; v++) {
            R5[v] = (uint8_t)((v * 31 + 127) / 255);
            G6[v] = (uint8_t)((v * 63 + 127) / 255);
        }
}

static inline void put565(uint16_t *p, const uint8_t *c, int a) {
    uint8_t q[3];
    if (a >= 256) {
        q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
    } else {
        unsigned v = *p, r = v >> 11, g = (v >> 5) & 63, b = v & 31;
        q[0] = (uint8_t)(r << 3 | r >> 2); q[1] = (uint8_t)(g << 2 | g >> 4); q[2] = (uint8_t)(b << 3 | b >> 2);
        put(q, c, a);
    }
    *p = (uint16_t)(R5[q[0]] << 11 | G6[q[1]] << 5 | R5[q[2]]);
}

/* a convex shape: inside the circle (r > 0) and every edge; edges smoothed
 * over one pixel. (x0, y0)-(x1, y1): a box around it. */
static void convex(const Surf *sf, float cx, float cy, float r, const Edge *e, int ne, float x0, float y0, float x1,
                   float y1, const uint8_t *col) {
    int W = sf->c ? sf->c->w : sf->im.w, H = sf->c ? sf->c->h : sf->im.h;
    int ax = (int)floorf(x0) - 1, ay = (int)floorf(y0) - 1, bx = (int)ceilf(x1) + 1, by = (int)ceilf(y1) + 1;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    if (bx > W) bx = W;
    if (by > H) by = H;
    if (sf->c) r5_init();
    for (int y = ay; y < by; y++) {
        float py = y + 0.5f, dy = py - cy;
        for (int x = ax; x < bx; x++) {
            float px = x + 0.5f, d = 1e9f;
            if (r > 0) {
                float dx = px - cx;
                d = r - sqrtf(dx * dx + dy * dy);
                if (d <= -0.5f) continue;
            }
            for (int k = 0; k < ne && d > -0.5f; k++) {
                float v = e[k].nx * px + e[k].ny * py + e[k].d;
                if (v < d) d = v;
            }
            if (d <= -0.5f) continue;
            int a = d >= 0.5f ? 256 : (int)((d + 0.5f) * 256);
            if (sf->c) put565(&sf->c->px[(size_t)y * sf->c->w + x], col, a);
            else put(PX(sf->im, x, y), col, a);
        }
    }
}

static void circle_s(const Surf *sf, float cx, float cy, float r, const uint8_t *col) {
    convex(sf, cx, cy, r, NULL, 0, cx - r, cy - r, cx + r, cy + r, col);
}

/* a convex polygon (n corners, in order either way round) */
static void polygon_s(const Surf *sf, const float *xy, int n, const uint8_t *col) {
    Edge e[8];
    float mx = 0, my = 0, x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        mx += xy[2 * i] / n;
        my += xy[2 * i + 1] / n;
        x0 = fminf(x0, xy[2 * i]); x1 = fmaxf(x1, xy[2 * i]);
        y0 = fminf(y0, xy[2 * i + 1]); y1 = fmaxf(y1, xy[2 * i + 1]);
    }
    for (int i = 0; i < n; i++) {
        float ax = xy[2 * i], ay = xy[2 * i + 1], bx = xy[2 * ((i + 1) % n)], by = xy[2 * ((i + 1) % n) + 1];
        float ex = bx - ax, ey = by - ay, l = sqrtf(ex * ex + ey * ey);
        if (l < 1e-6f) l = 1e-6f;
        Edge k = {-ey / l, ex / l, 0};
        k.d = -(k.nx * ax + k.ny * ay);
        if (k.nx * mx + k.ny * my + k.d < 0) {
            k.nx = -k.nx; k.ny = -k.ny; k.d = -k.d;
        }
        e[i] = k;
    }
    convex(sf, 0, 0, 0, e, n, x0, y0, x1, y1, col);
}

/* a quarter of a circle: from angle a to a + 90 degrees (Pillow's pieslice:
 * clockwise from 3 o'clock) */
static void quarter_s(const Surf *sf, float cx, float cy, float r, float a, const uint8_t *col) {
    float ca = cosf(a * (float)M_PI / 180), sa = sinf(a * (float)M_PI / 180);
    Edge e[2] = {{-sa, ca, 0}, {ca, sa, 0}};
    for (int k = 0; k < 2; k++) e[k].d = -(e[k].nx * cx + e[k].ny * cy);
    convex(sf, cx, cy, r, e, 2, cx - r, cy - r, cx + r, cy + r, col);
}

static void rect_s(const Surf *sf, float x0, float y0, float x1, float y1, const uint8_t *col) {
    float p[8] = {x0, y0, x1, y0, x1, y1, x0, y1};
    polygon_s(sf, p, 4, col);
}

/* (the R,G,B picture versions, as before) */
static void circle(Img im, float cx, float cy, float r, const uint8_t *col) {
    Surf sf = {im, NULL};
    circle_s(&sf, cx, cy, r, col);
}
static void polygon(Img im, const float *xy, int n, const uint8_t *col) {
    Surf sf = {im, NULL};
    polygon_s(&sf, xy, n, col);
}
static void quarter(Img im, float cx, float cy, float r, float a, const uint8_t *col) {
    Surf sf = {im, NULL};
    quarter_s(&sf, cx, cy, r, a, col);
}
static void rect(Img im, float x0, float y0, float x1, float y1, const uint8_t *col) {
    Surf sf = {im, NULL};
    rect_s(&sf, x0, y0, x1, y1, col);
}

/* ------------------------------------------------------------ the menus' shapes */
static void rgb3(Rgb c, uint8_t *o) { o[0] = c.r; o[1] = c.g; o[2] = c.b; }

void ui_rect(Canvas *c, float x0, float y0, float x1, float y1, Rgb col) {
    Surf sf = {{0}, c};
    uint8_t k[3];
    rgb3(col, k);
    rect_s(&sf, x0, y0, x1, y1, k);
}
void ui_circle(Canvas *c, float cx, float cy, float r, Rgb col) {
    Surf sf = {{0}, c};
    uint8_t k[3];
    rgb3(col, k);
    circle_s(&sf, cx, cy, r, k);
}
void ui_poly(Canvas *c, const float *xy, int n, Rgb col) {
    Surf sf = {{0}, c};
    uint8_t k[3];
    rgb3(col, k);
    polygon_s(&sf, xy, n, k);
}
void ui_quarter(Canvas *c, float cx, float cy, float r, float a, Rgb col) {
    Surf sf = {{0}, c};
    uint8_t k[3];
    rgb3(col, k);
    quarter_s(&sf, cx, cy, r, a, k);
}

/* the little "0 1" mark (the menus' headers): its ring's middle at (cx, cy), radius r */
void ui_mark(Canvas *c, float cx, float cy, float r, Rgb bg) {
    ui_circle(c, cx, cy, r, ZI_RED);
    ui_circle(c, cx, cy, r * .42f, bg);
    float bx = cx + r + 3;
    ui_rect(c, bx, cy - r, bx + r * .5f, cy + r, ZI_INK);
    float flag[6] = {bx, cy - r, bx, cy - r + r * .5f, bx - r * .44f, cy - r + r * .5f};
    ui_poly(c, flag, 3, ZI_INK);
}

/* (v53) the whole mark (the start-up picture's - gen_brand.py's mark01) */
void ui_logo(Canvas *c, float x, float y, float s, Rgb bg) {
    float r = 60 * s, bx = x + r + 22 * s;
    ui_circle(c, x, y, r, ZI_RED);                  /* 0 */
    ui_circle(c, x, y, r * .40f, bg);
    ui_rect(c, bx, y - r, bx + 30 * s, y + r, ZI_INK);   /* 1 */
    float flag[6] = {bx, y - r, bx, y - r + 30 * s, bx - 26 * s, y - r + 30 * s};
    ui_poly(c, flag, 3, ZI_INK);
    ui_quarter(c, bx + 30 * s, y + r, 42 * s, 270, ZI_YEL);
    ui_rect(c, bx + 46 * s, y + r * .24f, bx + 76 * s, y + r * .24f + 30 * s, ZI_BLU);
    for (int k = -1; k <= 1; k += 2) {              /* Zero peeking */
        float ex = x + k * 14 * s, ey = y - r * .70f, er = 5.2f * s;
        ui_circle(c, ex, ey, er, ZI_INK);
        ui_circle(c, ex - er * .32f, ey - er * .35f, er * .36f, bg);
    }
}

float ui_logo_width(float s) { return 60 * s * 2 + 22 * s + 76 * s; }

void ui_art(Canvas *c, const BrandArt *a, int x, int y) {
    r5_init();
    for (int j = 0; j < a->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= c->h) continue;
        const uint8_t *s = a->rgba + (size_t)j * a->w * 4;
        for (int i = 0; i < a->w; i++, s += 4) {
            int xx = x + i;
            if (xx < 0 || xx >= c->w || !s[3]) continue;
            put565(&c->px[(size_t)yy * c->w + xx], s, s[3] == 255 ? 256 : s[3]);
        }
    }
}

/* a picture with its own transparency (brandart.c) at (x, y) */
static void art(Img im, const BrandArt *a, int x, int y) {
    for (int j = 0; j < a->h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= im.h) continue;
        const uint8_t *s = a->rgba + (size_t)j * a->w * 4;
        for (int i = 0; i < a->w; i++, s += 4) {
            int xx = x + i;
            if (xx < 0 || xx >= im.w || !s[3]) continue;
            put(PX(im, xx, yy), s, s[3] == 255 ? 256 : s[3]);
        }
    }
}

static void to_canvas(Img im, Canvas *c) {
    r5_init();
    for (int y = 0; y < c->h && y < im.h; y++) {
        const uint8_t *p = PX(im, 0, y);
        uint16_t *o = c->px + (size_t)y * c->w;
        for (int x = 0; x < c->w && x < im.w; x++, p += 3)
            o[x] = (uint16_t)(R5[p[0]] << 11 | G6[p[1]] << 5 | R5[p[2]]);
    }
}

static Img work;                                   /* the R,G,B picture drawn into */

static Img work_for(int w, int h) {
    if (work.p && (work.w != w || work.h != h)) img_free(&work);
    if (!work.p) work = img_alloc(w, h, 3);
    return work;
}

void saver_free(void) {
    if (work.p) img_free(&work);
}

static void fill(Img im, const uint8_t *c) {
    for (int y = 0; y < im.h; y++) {
        uint8_t *q = PX(im, 0, y);
        for (int x = 0; x < im.w; x++, q += 3) {
            q[0] = c[0]; q[1] = c[1]; q[2] = c[2];
        }
    }
}

/* ------------------------------------------------------------ the screen saver */
enum { SH_CIRCLE, SH_QUARTER, SH_SQUARE, SH_BAR, SH_TRI, SH_RING };
/* the design's eight shapes: where each starts (px, py: parts of the screen)
 * and which way it drifts (vx, vy) - the design's random.Random(5) */
static const struct {
    int kind;
    const uint8_t *col;
    float size;
    double px, py, vx, vy;
} SHAPES[8] = {
    {SH_CIRCLE, RED, 24, 0.6229016948897019, 0.7417869892607294, 0.5903871311313933, 0.8849005675541006},
    {SH_QUARTER, YEL, 38, 0.7398985747399307, 0.922324996665417, -0.9419895434327705, -0.0687546912437893},
    {SH_SQUARE, BLU, 20, 0.9433567169983137, 0.6489745531369242, 0.8018009835012454, -0.7735880706937113},
    {SH_BAR, CREAM, 40, 0.46906904778216374, 0.24657283261983032, 0.08752171847186085, 0.14788237585620156},
    {SH_TRI, RED, 26, 0.013114189588902203, 0.21672980046384815, -0.44103526797777937, 0.8326907436171038},
    {SH_CIRCLE, YEL, 16, 0.7657254516291417, 0.15960421235803823, 0.594293982862409, -0.7224651632021937},
    {SH_RING, BLU, 20, 0.6174525204661166, 0.1266992325502697, -0.9964502755949307, 0.7428094894485642},
    {SH_SQUARE, CREAM, 10, 0.2094563824951179, 0.21548116922473226, 0.9648422176518505, 0.7448155308736037},
};
static const float CORNERS[3][8] = {{-1, -1, 1, -1, 1, 1, -1, 1},              /* square */
                                    {-.22f, -1, .22f, -1, .22f, 1, -.22f, 1},  /* bar */
                                    {-1, .8f, 1, .8f, 0, -.95f, 0, 0}};        /* triangle */

static double wrap(double v, double m) {           /* Python's v % m */
    double r = fmod(v, m);
    return r < 0 ? r + m : r;
}

void saver_draw(Canvas *c, double seconds) {
    Img im = work_for(c->w, c->h);
    int w = c->w, h = c->h;
    double t = seconds * SAVER_SPEED;
    fill(im, BLK);
    for (int i = 0; i < 8; i++) {
        double sp = 0.018;                          /* (wrapping around past the edges) */
        float x = (float)((wrap(SHAPES[i].px + SHAPES[i].vx * sp * t, 1.4) - .2) * w);
        float y = (float)((wrap(SHAPES[i].py + SHAPES[i].vy * sp * t, 1.4) - .2) * h);
        float a = (float)wrap(t * 6 * (i % 2 ? 1 : -1) + i * 40, 360);
        float s = SHAPES[i].size;
        switch (SHAPES[i].kind) {
        case SH_CIRCLE: circle(im, x, y, s, SHAPES[i].col); break;
        case SH_RING:
            circle(im, x, y, s, SHAPES[i].col);
            circle(im, x, y, s * .45f, BLK);
            break;
        case SH_QUARTER: quarter(im, x, y, s, a, SHAPES[i].col); break;
        default: {
            const float *k = CORNERS[SHAPES[i].kind - SH_SQUARE];
            int n = SHAPES[i].kind == SH_TRI ? 3 : 4;
            float ca = cosf(a * (float)M_PI / 180), sa = sinf(a * (float)M_PI / 180), p[8];
            for (int j = 0; j < n; j++) {
                p[2 * j] = x + s * (k[2 * j] * ca - k[2 * j + 1] * sa);
                p[2 * j + 1] = y + s * (k[2 * j] * sa + k[2 * j + 1] * ca);
            }
            polygon(im, p, n, SHAPES[i].col);
        }
        }
    }
    art(im, &BR_MARK, w / 2 + BR_MARK.dx, h / 2 + BR_MARK.dy);
    art(im, &BR_TEXT, w / 2 + BR_TEXT.dx, h + BR_TEXT.dy);
    to_canvas(im, c);
}

/* ------------------------------------------------------------ the start-up picture */
void splash_draw(Canvas *c, const char *version) {
    int w = c->w, h = c->h, wide = w > h;
    Img im = work_for(w, h);
    fill(im, CREAM);                                /* (v54: plain cream - no thin lines) */
    float s = wide ? .92f : .78f, x = w * (wide ? .36f : .34f), y = h * (wide ? .38f : .36f), r = 60 * s;
    circle(im, x, y, r, RED);                       /* the mark: 0 */
    circle(im, x, y, r * .40f, CREAM);
    float bx = x + r + 22 * s;                      /* 1 */
    rect(im, bx, y - r, bx + 30 * s, y + r, BLK);
    float flag[6] = {bx, y - r, bx, y - r + 30 * s, bx - 26 * s, y - r + 30 * s};
    polygon(im, flag, 3, BLK);
    quarter(im, bx + 30 * s, y + r, 42 * s, 270, YEL);
    rect(im, bx + 46 * s, y + r * .24f, bx + 76 * s, y + r * .24f + 30 * s, BLU);
    for (int k = -1; k <= 1; k += 2) {              /* Zero peeking */
        float ex = x + k * 14 * s, ey = y - r * .70f, er = 5.2f * s;
        circle(im, ex, ey, er, BLK);
        circle(im, ex - er * .32f, ey - er * .35f, er * .36f, CREAM);
    }
    to_canvas(im, c);
    Rgb ink = {BLK[0], BLK[1], BLK[2]}, grey = {120, 112, 98};
    cv_text(c, wide ? 30 : 22, w * .125, h * .83, "zero ichi", ink, "lm", 0);
    char t[64];
    snprintf(t, sizeof t, "v%s \xe2\x80\xa2 %dx%d", version, w, h);
    cv_text(c, 12, w * .125, h * .94, "starting", grey, "lm", 0);
    cv_text(c, 12, w * .875, h * .94, t, grey, "rm", 0);
}
