/* Zero Ichi - the looks: a port of the Python Zero Ichi's filters.py (same steps,
 * same tables - see tables.c - and Pillow's maths, see img.c). */
#include "looks.h"
#include "ycc_inline.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

const char *const LOOK_NAMES[NLOOKS] = {
    "Normal", "Super 8", "Holga B&W", "Holga Color", "GB Camera", "GB Camera B&W",
    "16-Bit Arcade", "16-Bit Neon", "16-Bit Paper", "Mono", "Hi-Con B&W", "Cross", "Thermal", "Prism", "Prism 3",
    "Prism Rainbow", "Zero Ichi"};

int looks_test_mode = 0;

int look_by_name(const char *name) {
    for (int i = 0; i < NLOOKS; i++)
        if (!strcmp(name, LOOK_NAMES[i])) return i;
    static const char *const old[][2] = {
        {"GameBoy", "GB Camera"}, {"8-Bit", "16-Bit Arcade"}, {"16-Bit", "16-Bit Arcade"},
        {"Harinezumi", "Normal"}, {"Lomo", "Normal"}, {"Sepia", "Mono"}, {"Negative", "Normal"},
        {"Kaleidoscope", "Prism"}, {"8mm", "Super 8"}, {"GB Color", "GB Camera"}};
    for (unsigned i = 0; i < sizeof old / sizeof old[0]; i++)
        if (!strcasecmp(name, old[i][0])) return look_by_name(old[i][1]);   /* (any case: presets) */
    return -1;
}

int look_is_tone(int id) {
    return id == LOOK_HOLGA || id == LOOK_GB || id == LOOK_GB_BW || id == LOOK_MONO || id == LOOK_HICON ||
           id == LOOK_THERMAL;
}
int look_is_pixel(int id) { return id >= LOOK_PX_ARCADE && id <= LOOK_PX_PAPER; }
int look_is_yuv(int id) { return id == LOOK_NORMAL || id == LOOK_CROSS; }
int look_half_ok(int id) { return id == LOOK_8MM || id == LOOK_GB || id == LOOK_GB_BW || look_is_pixel(id); }
int look_square(int id) { return id == LOOK_HOLGA || id == LOOK_HOLGA_COLOR; }

/* ------------------------------------------------------------- helpers */
int pyround(double v) { return (int)rint(v); }       /* halves to even, like Python */

static uint32_t rng_state = 0;
static pthread_mutex_t rng_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t rnd_u32(void) {
    pthread_mutex_lock(&rng_lock);
    if (!rng_state) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        rng_state = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec << 16)) | 1u;
    }
    uint32_t x = rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state = x;
    pthread_mutex_unlock(&rng_lock);
    return x;
}
double looks_test_dice = 0.5;                       /* (tests: what the dice say; < 0: a */
static uint64_t test_lcg = 12345;                   /*  fixed sequence, the tests' own) */
void looks_test_seed(unsigned seed) { test_lcg = seed; }
double rnd01(void) {
    if (looks_test_mode && looks_test_dice < 0) {
        test_lcg = (test_lcg * 1103515245u + 12345u) & 0x7fffffffu;
        return test_lcg / 2147483648.0;
    }
    if (looks_test_mode) return looks_test_dice;
    return (rnd_u32() >> 8) * (1.0 / 16777216.0);
}
int rnd_pick(int n) { return (int)(rnd01() * n); }

static double rnd_normal(void) {               /* Box-Muller */
    double u1 = ((rnd_u32() >> 8) + 1) * (1.0 / 16777217.0), u2 = (rnd_u32() >> 8) * (1.0 / 16777216.0);
    return sqrt(-2.0 * log(u1)) * cos(2 * M_PI * u2);
}

static Img luma(Img a) { return img_to_l(a); }

/* ---------------------------------------------- masks (kept per thread) */
typedef struct {
    int kind, w, h, power, gate, c;
    float strength;
    Img img;
    unsigned used;
} Mask;
#define NMASK 16
static __thread Mask masks[NMASK];
static __thread unsigned mask_clock;

static Mask *mask_find(int kind, int w, int h, float strength, int power, int gate, int c) {
    for (int i = 0; i < NMASK; i++) {
        Mask *m = &masks[i];
        if (m->img.p && m->kind == kind && m->w == w && m->h == h && m->strength == strength &&
            m->power == power && m->gate == gate && m->c == c) {
            m->used = ++mask_clock;
            return m;
        }
    }
    return NULL;
}

static Mask *mask_slot(void) {
    Mask *best = &masks[0];
    for (int i = 0; i < NMASK; i++) {
        if (!masks[i].img.p) return &masks[i];
        if (masks[i].used < best->used) best = &masks[i];
    }
    img_free(&best->img);
    return best;
}

/* filters._smooth_mask: worked out at quarter size (float32, like numpy) and
 * enlarged with Pillow's bilinear resize */
enum { FN_VIG, FN_FOCUS };
static Img smooth_mask(int w, int h, int fn, float strength, int power) {
    int f = (w < h ? w : h) >= 128 ? 4 : 1;
    int sw = (w + f - 1) / f, sh = (h + f - 1) / f;
    Img g = img_new(sw, sh, 1);
    float hy = (float)(sh / 2.0), hx = (float)(sw / 2.0);
    float r2 = (float)sqrt(2.0);
    for (int yy = 0; yy < sh; yy++) {
        volatile float y = (((float)yy + 0.5f) - hy) / hy;
        for (int xx = 0; xx < sw; xx++) {
            volatile float x = (((float)xx + 0.5f) - hx) / hx;
            volatile float y2 = y * y, x2 = x * x;
            volatile float s = y2 + x2;
            volatile float d = sqrtf(s) / r2;
            float v;
            if (fn == FN_VIG) {
                volatile float p = powf(d, 2.2f);
                volatile float t = strength * p;
                volatile float q = 1.0f - t;
                v = power == 2 ? q * q : q;
            } else {
                volatile float t = (0.80f - d) / 0.45f;
                v = t < 0 ? 0 : t > 1 ? 1 : t;
            }
            volatile float o = v * 255.0f + 0.5f;
            int iv = o <= 0 ? 0 : o >= 255 ? 255 : (int)o;
            g.p[yy * g.stride + xx] = (uint8_t)iv;
        }
    }
    if (sw == w && sh == h) return g;
    Img out = img_resize(g, w, h, RS_BILINEAR);
    img_free(&g);
    return out;
}

/* filters._gate: rounded corners and a thin black edge (0 / 255) */
static Img gate_l(int w, int h) {
    Img m = img_new(w, h, 1);
    memset(m.p, 255, (size_t)w * h);
    int r = (int)((w < h ? w : h) * 0.08);
    int e = w / 120 > 1 ? w / 120 : 1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (y < e || y >= h - e || x < e || x >= w - e) m.p[y * w + x] = 0;
    if (r > 0) {
        for (int yy = 0; yy < r; yy++)
            for (int xx = 0; xx < r; xx++) {
                int out = (r - yy) * (r - yy) + (r - xx) * (r - xx) > r * r;
                if (!out) continue;
                m.p[(e + yy) * w + (e + xx)] = 0;                               /* top left */
                m.p[(e + yy) * w + (w - e - 1 - xx)] = 0;                       /* top right */
                m.p[(h - e - 1 - yy) * w + (e + xx)] = 0;                       /* bottom left */
                m.p[(h - e - 1 - yy) * w + (w - e - 1 - xx)] = 0;               /* bottom right */
            }
    }
    return m;
}

static Img vig_mask(int w, int h, float strength, int c, int power, int gate) {
    Mask *m = mask_find(FN_VIG, w, h, strength, power, gate, c);
    if (m) return m->img;
    Img g = smooth_mask(w, h, FN_VIG, strength, power);
    if (gate) {
        Img gl = gate_l(w, h);
        img_multiply(g, gl);
        img_free(&gl);
    }
    if (c == 3) {
        Img rgb = img_to_rgb(g);
        img_free(&g);
        g = rgb;
    }
    m = mask_slot();
    *m = (Mask){FN_VIG, w, h, power, gate, c, strength, g, ++mask_clock};
    return m->img;
}

static Img focus_mask(int w, int h) {
    Mask *m = mask_find(FN_FOCUS, w, h, 0, 0, 0, 1);
    if (m) return m->img;
    Img g = smooth_mask(w, h, FN_FOCUS, 0, 0);
    m = mask_slot();
    *m = (Mask){FN_FOCUS, w, h, 0, 0, 1, 0, g, ++mask_clock};
    return m->img;
}

/* -------------------------------------------------------------- grain */
static pthread_mutex_t grain_lock = PTHREAD_MUTEX_INITIALIZER;
static Img grain_base[4];   /* [0] amp 14 (Holga), [1] amp 12 (Super 8), [2] 30 (Hi-Con), [3] 9 (Zero Ichi) */
static int grain_amp[4] = {14, 12, 30, 9};       /* (v45: about half of before - "less grain") */

static Img grain_get(int amp) {
    int i = amp == 14 ? 0 : amp == 12 ? 1 : amp == 9 ? 3 : 2;
    pthread_mutex_lock(&grain_lock);
    if (!grain_base[i].p) {
        Img g = img_new(512, 512, 1);
        double sd = grain_amp[i] / 2.2;
        for (int k = 0; k < 512 * 512; k++) {
            double n = rint(rnd_normal() * sd);
            if (n < -amp) n = -amp;
            if (n > amp) n = amp;
            g.p[k] = (uint8_t)(((int)n) & 255);
        }
        grain_base[i] = g;
    }
    pthread_mutex_unlock(&grain_lock);
    return grain_base[i];
}

/* filters._grain_mod: film grain to add modulo 256 (random crop) */
static Img grain_mod(int w, int h, int amp, int c) {
    Img o = img_new(w, h, c);
    if (looks_test_mode) return o;                /* (zeros: no grain) */
    if (w > 512 || h > 512) {                     /* big photos: coarser grain */
        int mx = w > h ? w : h;
        int f = (mx + 511) / 512;
        img_free(&o);
        Img t = grain_mod((w + f - 1) / f, (h + f - 1) / f, amp, c);
        o = img_resize(t, w, h, RS_NEAREST);
        img_free(&t);
        return o;
    }
    Img base = grain_get(amp);
    int x0 = rnd_pick(513 - w), y0 = rnd_pick(513 - h);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = base.p + (y0 + y) * 512 + x0;
        uint8_t *d = PX(o, 0, y);
        if (c == 1) {
            memcpy(d, s, w);
        } else {
            for (int x = 0; x < w; x++, d += 3) d[0] = d[1] = d[2] = s[x];
        }
    }
    return o;
}

/* filters._bayer_img: ordered dither pattern (128 = no change) */
static const float BAYER4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
static Img bayer_img(int w, int h, float amount) {
    Img o = img_new(w, h, 1);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            volatile float b = (BAYER4[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
            volatile float v = 128.0f + b * amount;
            int iv = v <= 0 ? 0 : v >= 255 ? 255 : (int)v;
            o.p[y * w + x] = (uint8_t)iv;
        }
    return o;
}

/* ------------------------------------------------------------- tones */
static Tone tone_new(Img t, const Palette *pal, int w, int h) {
    Tone tn = {t, pal, w ? w : t.w, h ? h : t.h, 0, 0};
    return tn;
}

void tone_free(Tone *t) { img_free(&t->t); }

const Palette *tone_palette(Tone *t) { return t->pal ? t->pal : &PAL_GRAY; }

/* filters._soften */
/* soften's small picture (to_w wide) - enlarged again it is the soft one */
static Img soften_small(Img g, int w, int h, int to_w) {
    int th = pyround((double)h * to_w / w);
    if (th < 1) th = 1;
    Img small = g, red = {0};
    int f = w / (to_w * 2);
    if (f >= 2) {
        red = img_reduce(g, f);
        small = red;
    }
    Img a = img_resize(small, to_w, th, RS_BILINEAR);
    if (red.p) img_free(&red);
    return a;
}

static Img soften(Img g, int w, int h, int to_w) {
    Img a = soften_small(g, w, h, to_w);
    Img b = img_resize(a, w, h, RS_BILINEAR);
    img_free(&a);
    return b;
}

static const float GB_SHARPEN[9] = {-1, -1, -1, -1, 12, -1, -1, -1, -1};

Tone tone_make(int id, Img src, int ow, int oh) {
    if (id == LOOK_GB || id == LOOK_GB_BW) {
        int W0 = src.w, H0 = src.h;
        int gw = W0 >= H0 ? 128 : (pyround(128.0 * W0 / H0) > 8 ? pyround(128.0 * W0 / H0) : 8);
        int gh = pyround((double)gw * H0 / W0);
        if (gh < 8) gh = 8;
        Img s = img_shrink_fast(src, gw, gh);
        if (s.c != 1) {
            Img l = img_to_l(s);
            img_free(&s);
            s = l;
        }
        Img g = img_filter3x3(s, GB_SHARPEN, 4.0f, 0.0f);
        img_free(&s);
        img_point(g, T_GB_CONTRAST);
        Img b = bayer_img(gw, gh, (float)(85 * 0.95));
        img_add(g, b, 1.0f, -128);
        img_free(&b);
        Tone tn = tone_new(g, id == LOOK_GB ? &PAL_GB_GREEN : &PAL_GB_GRAY, ow ? ow : W0, oh ? oh : H0);
        tn.sharp = 1;
        return tn;
    }
    Img g = luma(src);
    int W0 = g.w, H0 = g.h;
    if (id == LOOK_HOLGA) {
        if (W0 > 110) {
            Img soft = soften(g, W0, H0, 110);
            img_composite(g, soft, focus_mask(W0, H0));
            img_free(&soft);
        }
        img_point(g, T_HOLGA_C);
        Img gr = grain_mod(W0, H0, 14, 1);
        img_add_modulo(g, gr);
        img_free(&gr);
        img_multiply(g, vig_mask(W0, H0, 0.55f, 1, 2, 1));
        return tone_new(g, &PAL_HOLGA, ow, oh);
    }
    if (id == LOOK_HICON) {                         /* filters._hicon_tone (0.1.0: sharp) */
        img_point(g, T_HICON);                     /* (v48: no grain, a subtle vignette) */
        img_multiply(g, vig_mask(W0, H0, 0.30f, 1, 1, 0));
        return tone_new(g, NULL, ow, oh);
    }
    if (id == LOOK_MONO) {
        img_point(g, T_MONO);
        img_multiply(g, vig_mask(W0, H0, 0.35f, 1, 1, 0));
        return tone_new(g, NULL, ow, oh);
    }
    /* Thermal */
    return tone_new(g, &PAL_THERMAL, ow, oh);
}

static Img tone_sized(Tone *t, Img im, int w, int h, int smooth) {
    if (im.w == w && im.h == h) return img_copy(im);
    return img_resize(im, w, h, smooth && !t->sharp ? RS_BILINEAR : RS_NEAREST);
}

Img tone_rgb(Tone *t) {
    const Palette *p = t->pal;
    if (!p) {
        Img s = tone_sized(t, t->t, t->w, t->h, 0);
        Img o = img_to_rgb(s);
        img_free(&s);
        return o;
    }
    Img s = tone_sized(t, t->t, t->w, t->h, 0);
    Img o = img_new(s.w, s.h, 3);
    for (int y = 0; y < s.h; y++) {
        const uint8_t *a = PX(s, 0, y);
        uint8_t *q = PX(o, 0, y);
        for (int x = 0; x < s.w; x++, q += 3) {
            int v = a[x];
            q[0] = p->r[v];
            q[1] = p->g[v];
            q[2] = p->b[v];
        }
    }
    img_free(&s);
    return o;
}

void tone_yuv(Tone *t, int w, int h, int smooth, Img *Y, Img *U, Img *V) {
    if (!w) { w = t->w; h = t->h; }
    int hw = w / 2, hh = h / 2;
    const Palette *p = t->pal;
    int small = t->t.w != w || t->t.h != h;
    Img y = img_copy(t->t);
    if (p) img_point(y, p->y);
    *Y = tone_sized(t, y, w, h, smooth);
    img_free(&y);
    if (!p || p->gray) {
        *U = img_new(hw, hh, 1);
        *V = img_new(hw, hh, 1);
        memset(U->p, 128, (size_t)hw * hh);
        memset(V->p, 128, (size_t)hw * hh);
        return;
    }
    Img half;
    if (smooth && small && !t->indexed && !t->sharp && t->t.w > hw && t->t.h > hh && !((t->t.w | t->t.h) & 1))
        half = img_reduce(t->t, 2);                  /* (videos: halved, then enlarged - quick) */
    else if (small || t->indexed)
        half = img_copy(t->t);
    else if (w % 2 == 0 && h % 2 == 0)
        half = img_reduce(t->t, 2);
    else
        half = img_resize(t->t, hw, hh, RS_BOX);
    Img u = img_copy(half), v = img_copy(half);
    img_point(u, p->u);
    img_point(v, p->v);
    *U = tone_sized(t, u, hw, hh, smooth);
    *V = tone_sized(t, v, hw, hh, smooth);
    img_free(&u);
    img_free(&v);
    img_free(&half);
}

Img tone_gif(Tone *t, const Palette **pal) {
    const Palette *p = tone_palette(t);
    *pal = p;
    Img g = img_copy(t->t);
    img_point(g, p->gif_index);
    Img o = tone_sized(t, g, t->w, t->h, 0);
    img_free(&g);
    return o;
}

/* -------------------------------------------------------- 16-Bit (pixel art) */
typedef struct {
    const Palette *pal;
    const uint8_t *lut;
    int amp;
    uint16_t ty[256 + 64], tu[256], tv[256];
} PixelPal;
static PixelPal pixpal[3];

static const PixelPal *pixel_pal(int id) {
    return &pixpal[id - LOOK_PX_ARCADE];
}

void pixel_grid(int w, int h, int *gw, int *gh) {
    double mx = w > h ? w : h, mn = w < h ? w : h;
    double s = 1.0;
    if (160.0 / mx < s) s = 160.0 / mx;
    if (120.0 / mn < s) s = 120.0 / mn;
    *gw = pyround(w * s);
    *gh = pyround(h * s);
    if (*gw < 8) *gw = 8;
    if (*gh < 8) *gh = 8;
}

static uint16_t dither_value(int x, int y, int amp) {
    volatile float t = BAYER4[y & 3][x & 3];
    volatile float v = (t + 0.5f) * (float)(2 * amp / 16.0);
    return (uint16_t)nearbyintf(v);
}

static Tone pixel_index(int id, Img Y, Img U, Img V, int half, int W0, int H0, int ow, int oh) {
    const PixelPal *pp = pixel_pal(id);
    int gw = Y.w, gh = Y.h;
    Img idx = img_new(gw, gh, 1);
    for (int y = 0; y < gh; y++) {
        const uint8_t *py = PX(Y, 0, y);
        const uint8_t *pu = half ? PX(U, 0, y / 2) : PX(U, 0, y);
        const uint8_t *pv = half ? PX(V, 0, y / 2) : PX(V, 0, y);
        uint8_t *o = PX(idx, 0, y);
        for (int x = 0; x < gw; x++) {
            int xu = half ? x / 2 : x;
            int uv = pp->tu[pu[xu]] + pp->tv[pv[xu]];
            int d = dither_value(x, y, pp->amp);
            o[x] = pp->lut[pp->ty[py[x] + d] + uv];
        }
    }
    Tone tn = tone_new(idx, pp->pal, ow ? ow : W0, oh ? oh : H0);
    tn.indexed = 1;
    tn.sharp = 1;
    return tn;
}

Tone pixel_tone_planes(int id, Img Y, Img U, Img V, int ow, int oh) {
    int gw, gh;
    pixel_grid(Y.w, Y.h, &gw, &gh);
    if (Y.w == gw && Y.h == gh && U.w == gw / 2 && U.h == gh / 2 && gw % 2 == 0 && gh % 2 == 0)
        return pixel_index(id, Y, U, V, 1, Y.w, Y.h, ow, oh);
    Img ys = img_shrink_fast(Y, gw, gh), us = img_shrink_fast(U, gw, gh), vs = img_shrink_fast(V, gw, gh);
    Tone t = pixel_index(id, ys, us, vs, 0, Y.w, Y.h, ow, oh);
    img_free(&ys); img_free(&us); img_free(&vs);
    return t;
}

Tone pixel_tone_rgb(int id, Img rgb, int ow, int oh) {
    int gw, gh;
    pixel_grid(rgb.w, rgb.h, &gw, &gh);
    Img s = (rgb.w == gw && rgb.h == gh) ? img_copy(rgb) : img_shrink_fast(rgb, gw, gh);
    if (s.c == 1) {
        Img c3 = img_to_rgb(s);
        img_free(&s);
        s = c3;
    }
    Img y, u, v;
    img_rgb_to_ycc(s, &y, &u, &v);
    Tone t = pixel_index(id, y, u, v, 0, rgb.w, rgb.h, ow, oh);
    img_free(&s); img_free(&y); img_free(&u); img_free(&v);
    return t;
}

/* ---------------------------------------------------------------- Super 8 */
#define S8_SAT 310
static const int S8_LEAK[3] = {255, 110, 30};

/* filters._S8Film: a moving film - in videos, GIFs and the live view the
 * flicker, weave and light leak carry on from frame to frame and change
 * gently (photos: one frame of their own). No scratches or dust (v48). Same
 * dice, same order as filters._s8_next. */
typedef struct { int k, dx, dy, leak, lside, lstr; } S8Frame;
static struct { int k, dx, dy, ln, ll, ls, lst; } s8 = {8, 0, 0, 0, 0, 0, 0};
static pthread_mutex_t s8_lock = PTHREAD_MUTEX_INITIALIZER;

static int clamp1(int v) { return v < -1 ? -1 : v > 1 ? 1 : v; }

static S8Frame s8_next(int photo) {
    S8Frame f = {0};
    if (photo) {
        double gain = 0.90 + 0.16 * rnd01();
        int k = pyround(gain * 100) - 90;
        f.k = k < 0 ? 0 : k > 16 ? 16 : k;
        if (rnd01() < 0.4) {                       /* a light leak in 2 of 5 photos */
            f.leak = 1;
            f.lside = rnd_pick(2);
            f.lstr = 170 + rnd_pick(86);
        }
        return f;
    }
    pthread_mutex_lock(&s8_lock);
    s8.k = (s8.k * 3 + 4 + rnd_pick(9) + 2) >> 2;
    if (rnd01() < 0.10) s8.dx = clamp1(s8.dx + rnd_pick(3) - 1);
    if (rnd01() < 0.06) s8.dy = clamp1(s8.dy + rnd_pick(3) - 1);
    f.k = s8.k;
    f.dx = s8.dx;
    f.dy = s8.dy;
    if (s8.ln == 0 && rnd01() < 0.008) {
        s8.ls = rnd_pick(2);
        s8.lst = 170 + rnd_pick(86);
        s8.ll = 10 + rnd_pick(16);
        s8.ln = s8.ll;
    }
    if (s8.ln > 0) {
        int pos = s8.ll - s8.ln, a = pos + 1 < s8.ln ? pos + 1 : s8.ln;
        int e = a * 512 / s8.ll;
        if (e > 256) e = 256;
        f.leak = 1;
        f.lside = s8.ls;
        f.lstr = s8.lst * e >> 8;
        s8.ln--;
    }
    pthread_mutex_unlock(&s8_lock);
    return f;
}

/* grain_mod's numbers for a w x h picture, without making the picture: rows of
 * the 512 x 512 grain (NULL: none - tests) */
static const uint8_t *grain_rows(int w, int h, int amp) {
    if (looks_test_mode || w > 512 || h > 512) return NULL;
    Img base = grain_get(amp);
    int x0 = rnd_pick(513 - w), y0 = rnd_pick(513 - h);
    return base.p + (size_t)y0 * 512 + x0;
}

/* x / 255 for 0 <= x <= 65025, exactly (no division) */
static inline unsigned div255(unsigned x) { return (x + 1 + (x >> 8)) >> 8; }

/* The richer colours' result, clipped to leave room for the grain:
 * S8_CLIP[gr + ((c - gr) * S8_SAT >> 8) + S8_OFF] */
#define S8_OFF 320
static uint8_t S8_CLIP[S8_OFF + 256 + S8_OFF];
static pthread_once_t s8_once = PTHREAD_ONCE_INIT;
static void s8_tables(void) {
    for (int i = 0; i < (int)sizeof S8_CLIP; i++) {
        int v = i - S8_OFF;
        S8_CLIP[i] = (uint8_t)(v < 24 ? 24 : v > 231 ? 231 : v);
    }
}

/* one run of pixels of a row: src -> dst (see eightmm) */
static inline void s8_run(uint8_t *q, const uint8_t *p, const uint8_t *gn, const uint8_t *mr,
                          const uint8_t *lc, int n, const uint8_t *t) {
    const uint8_t *cl = S8_CLIP + S8_OFF;
    for (int i = 0; i < n; i++, p += 3, q += 3) {
        int r = t[p[0]], g = t[256 + p[1]], b = t[512 + p[2]];
        int gr = (r * 77 + g * 150 + b * 29) >> 8;
        unsigned m = mr[i], g8 = gn[i];
        unsigned vr = (uint8_t)(cl[gr + (((r - gr) * S8_SAT) >> 8)] + g8);   /* (grain: wraps, as Pillow's) */
        unsigned vg = (uint8_t)(cl[gr + (((g - gr) * S8_SAT) >> 8)] + g8);
        unsigned vb = (uint8_t)(cl[gr + (((b - gr) * S8_SAT) >> 8)] + g8);
        if (lc) {                                  /* a light leak: screen blend */
            vr = 255 - div255((255 - vr) * (255u - lc[i * 3]));
            vg = 255 - div255((255 - vg) * (255u - lc[i * 3 + 1]));
            vb = 255 - div255((255 - vb) * (255u - lc[i * 3 + 2]));
        }
        q[0] = (uint8_t)div255(vr * m);            /* the film gate */
        q[1] = (uint8_t)div255(vg * m);
        q[2] = (uint8_t)div255(vb * m);
    }
}

/* filters.eightmm. Every step is per pixel, so they all run in one pass
 * (weave, colours and flicker, richer colours, grain, light leak, film gate) -
 * the same numbers as filters.py's separate steps. */
Img eightmm(Img src, int OW, int OH) {
    pthread_once(&s8_once, s8_tables);
    int W0 = src.w, H0 = src.h;
    if (!OW) { OW = W0; OH = H0; }
    int photo = OW > 640;
    /* (0.1.0: sharper - photos at half their size, GIFs up to 640 wide at
     *  their own; until then photos were softened to 170 pixels wide and
     *  worked at 320, clips at 320) */
    int ww = photo ? OW / 2 : (W0 > 640 ? W0 / 2 : W0);
    int wh = pyround((double)H0 * ww / W0);
    if (wh < 1) wh = 1;
    Img rgb = src.c == 3 ? src : img_to_rgb(src);
    Img work = rgb;
    if (photo && (ww != W0 || wh != H0)) {
        work = img_resize(rgb, ww, wh, RS_BILINEAR);
    } else if (ww != W0 || wh != H0) {
        work = img_shrink_fast(rgb, ww, wh);
    }
    S8Frame fr = s8_next(photo);
    const uint8_t *t = T_EM + fr.k * 768;
    const uint8_t *g0 = grain_rows(ww, wh, 12);
    size_t gst = 512;
    Img gbig = {0};
    if (!g0 && !looks_test_mode) {                  /* (bigger than 512: coarser grain, as grain_mod) */
        gbig = grain_mod(ww, wh, 12, 1);
        g0 = gbig.p;
        gst = (size_t)gbig.stride;
    }
    uint8_t *zero = g0 ? NULL : calloc((size_t)ww, 1);   /* (no grain: tests) */
    uint8_t *lc = NULL;                            /* the light leak, per column and colour */
    if (fr.leak && fr.lstr > 0) {
        lc = malloc((size_t)3 * ww);
        for (int x = 0; x < ww; x++) {
            int xx = fr.lside ? ww - 1 - x : x, t2 = 256 - xx * 384 / ww;
            if (t2 < 0) t2 = 0;
            int m = ((t2 * t2) >> 8) * fr.lstr >> 8;
            for (int c = 0; c < 3; c++) lc[x * 3 + c] = (uint8_t)(S8_LEAK[c] * m >> 8);
        }
    }
    Img mask = vig_mask(ww, wh, 0.55f, 1, 1, 1);   /* (grey: the same for all three colours) */
    Img im = img_alloc(ww, wh, 3);
    int dx = fr.dx, dy = fr.dy;
    /* the weave rolls the picture by dx (-1, 0, 1): out x <- in x - dx, wrapping */
    int a0 = dx > 0 ? dx : 0, a1 = dx < 0 ? ww + dx : ww;  /* (the part without the wrap) */
    for (int y = 0; y < wh; y++) {
        int sy = y - dy;
        sy = sy < 0 ? sy + wh : sy >= wh ? sy - wh : sy;
        const uint8_t *srow = PX(work, 0, sy);
        const uint8_t *gn = g0 ? g0 + (size_t)y * gst : zero;
        const uint8_t *mr = PX(mask, 0, y);
        uint8_t *q = PX(im, 0, y);
        if (a1 > a0)
            s8_run(q + a0 * 3, srow + (a0 - dx) * 3, gn + a0, mr + a0, lc ? lc + a0 * 3 : NULL, a1 - a0, t);
        for (int x = 0; x < a0; x++)               /* (the wrapped pixel) */
            s8_run(q + x * 3, srow + (x - dx + ww) * 3, gn + x, mr + x, lc ? lc + x * 3 : NULL, 1, t);
        for (int x = a1; x < ww; x++)
            s8_run(q + x * 3, srow + (x - dx - ww) * 3, gn + x, mr + x, lc ? lc + x * 3 : NULL, 1, t);
    }
    free(lc);
    free(zero);
    if (gbig.p) img_free(&gbig);
    if (work.p != rgb.p) img_free(&work);
    if (rgb.p != src.p) img_free(&rgb);
    if (ww != OW || wh != OH) {                    /* enlarged smoothly (no blocks) */
        Img o = img_resize(im, OW, OH, RS_BILINEAR);
        img_free(&im);
        im = o;
    }
    return im;
}

/* --------------------------------------------------------- Holga Color */
/* filters.holga_color_yuv: the Holga's lens on colour film - soft edges, dark
 * corners and film border, punchy contrast + grain on the brightness,
 * strongly saturated, slightly warm colours. Square (look_square).
 * (v55) Worked straight on the camera's Y, U, V - until v54 the camera's
 * picture was turned into R,G,B for it, the look turned that into Y, Cb, Cr
 * and back, and a video turned it into Y, U, V once more: about three times
 * the work, so the live view could stutter (and videos lost frames). The
 * same steps, on each plane at its own size (U, V: half size):
 *  - sharp in the middle, soft around: each plane averaged down by a whole
 *    number (k: the picture about 110-120 pixels wide - U, V: half that) and
 *    enlarged again, mixed in by the focus mask (until v54: shrunk to 110
 *    wide with Pillow's smooth resize - a few times the work, the same blur)
 *  - Y: the contrast table, grain, back to full range; U, V: the colour tables
 *  - the vignette and film border: Y darker, U, V towards grey (128) - the
 *    same as darkening R, G and B. (U, V: the darkest of the 2x2 pixels each
 *    covers - no colour on the border's edge.) */
#define HC_SOFT 110
#define FN_VIGMIN 7                                /* (a mask kind: vig_min) */

/* the vignette at U, V's size: the darkest of each 2x2 (kept, like the masks) */
static Img vig_min(Img vm, int uw, int uh) {
    Mask *m = mask_find(FN_VIGMIN, vm.w, vm.h, 0, uw, uh, 1);
    if (m) return m->img;
    Img g = img_new(uw, uh, 1);
    for (int y = 0; y < uh; y++)
        for (int x = 0; x < uw; x++) {
            int v = 255;
            for (int j = 0; j < 2; j++)
                for (int i = 0; i < 2; i++) {
                    int yy = 2 * y + j, xx = 2 * x + i;
                    if (yy >= vm.h) yy = vm.h - 1;
                    if (xx >= vm.w) xx = vm.w - 1;
                    int a = PX(vm, xx, yy)[0];
                    if (a < v) v = a;
                }
            PX(g, x, y)[0] = (uint8_t)v;
        }
    m = mask_slot();
    *m = (Mask){FN_VIGMIN, vm.w, vm.h, uw, uh, 1, 0, g, ++mask_clock};
    return m->img;
}

/* one plane: soft around (k: averaged down by k and enlarged again; 0:
 * none), through lut - and for Y the grain (gn: rows of gstride) and lut2 -
 * then the vignette vm (Y: darker; chroma: towards 128) */
static Img hc_plane(Img p, int k, const uint8_t *lut, const uint8_t *gn0, size_t gstride,
                    const uint8_t *lut2, Img vm, int chroma) {
    int w = p.w, h = p.h;
    Img small = {0}, soft = {0}, fm = {0};
    ImgRows *sr = NULL;
    if (k >= 2) {
        small = img_reduce(p, k);
        sr = img_rows_begin(small, w, h);
        if (!sr) soft = img_resize(small, w, h, RS_BILINEAR);
        fm = focus_mask(w, h);
    }
    Img o = img_alloc(w, h, 1);
    uint8_t *mixed = malloc((size_t)w + 16), *zero = calloc((size_t)w + 16, 1);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = PX(p, 0, y), *vr = PX(vm, 0, y);
        uint8_t *q = PX(o, 0, y);
        if (fm.p) {                                /* sharp in the middle, soft around */
            const uint8_t *m = PX(fm, 0, y), *sp = sr ? img_rows_get(sr, y) : PX(soft, 0, y);
            for (int x = 0; x < w; x++) {
                unsigned c = s[x], mm = m[x];
                if (mm != 255) {                   /* (Pillow's composite at 0 and 255: exactly */
                    unsigned t = sp[x] * (255 - mm) + c * mm + 128;   /*  the soft / the sharp pixel) */
                    c = mm ? ((t >> 8) + t) >> 8 : sp[x];
                }
                mixed[x] = (uint8_t)c;
            }
            s = mixed;
        }
        if (!chroma) {                             /* Y: contrast, grain (wraps, as Pillow's), darker */
            const uint8_t *gn = gn0 ? gn0 + (size_t)y * gstride : zero;
            for (int x = 0; x < w; x++)
                q[x] = (uint8_t)div255((unsigned)lut2[(uint8_t)(lut[s[x]] + gn[x])] * vr[x]);
        } else {                                   /* U, V: the colour table, towards grey */
            for (int x = 0; x < w; x++) {
                unsigned v = vr[x];
                q[x] = (uint8_t)div255(lut[s[x]] * v + 128 * (255 - v) + 127);
            }
        }
    }
    free(mixed);
    free(zero);
    img_rows_end(sr);
    if (soft.p) img_free(&soft);
    if (small.p) img_free(&small);
    return o;
}

/* Y, U, V (U, V half or full size) -> Holga Color's Y, U, V (new planes) */
void hc_yuv(Img Y, Img U, Img V, Img *oy, Img *ou, Img *ov) {
    int W0 = Y.w, H0 = Y.h;
    const uint8_t *g0 = grain_rows(W0, H0, 14);
    size_t gstride = 512;
    Img gbig = {0};
    if (!g0 && !looks_test_mode) {                 /* (big photos: grain_mod's coarser grain) */
        gbig = grain_mod(W0, H0, 14, 1);
        g0 = gbig.p;
        gstride = (size_t)gbig.stride;
    }
    Img vm = vig_mask(W0, H0, 0.55f, 1, 2, 1);
    int k = W0 > HC_SOFT ? (W0 / HC_SOFT < 2 ? 2 : W0 / HC_SOFT) : 0;   /* (the same k for U, V) */
    *oy = hc_plane(Y, k, T_HC_Y, g0, gstride, PAL_HOLGA.r, vm, 0);
    if (gbig.p) img_free(&gbig);
    Img vc = (U.w == W0 && U.h == H0) ? vig_mask(W0, H0, 0.55f, 1, 2, 1) : vig_min(vm, U.w, U.h);
    *ou = hc_plane(U, k, T_HC_CB, NULL, 0, NULL, vc, 1);
    *ov = hc_plane(V, k, T_HC_CR, NULL, 0, NULL, vc, 1);
}

/* (R,G,B pictures - GIFs: through Y, Cb, Cr at full size, as filters.holga_color) */
static Img holga_color(Img src) {
    Img in = src.c == 3 ? src : img_to_rgb(src);
    Img y, cb, cr, oy, ou, ov;
    img_rgb_to_ycc(in, &y, &cb, &cr);
    if (in.p != src.p) img_free(&in);
    hc_yuv(y, cb, cr, &oy, &ou, &ov);
    Img o = img_ycc_to_rgb(oy, ou, ov);
    img_free(&y); img_free(&cb); img_free(&cr); img_free(&oy); img_free(&ou); img_free(&ov);
    return o;
}

/* ---------------------------------------------------------------- Prism */
/* filters._prism: like shooting through a kaleidoscope prism filter - a clear
 * flat middle surrounded by slanted facets, each showing the picture moved
 * towards the middle, so the subject repeats in a ring around itself. Soft
 * facet edges, colours split a little by the glass, colours a little
 * stronger. Whole numbers only (exactly as filters.py, PRISMS). Sizes: 1024 =
 * half the picture's shorter side. */
typedef struct {
    int n, nx[6], ny[6];           /* facets: directions x 256, in order around the middle */
    int clear, ramp, most;         /* the clear middle, its soft edge, how much a facet shows */
    int shift, disp, soft, sat;    /* how far a facet moves the picture, colour split (%), ... */
} PrismSpec;
static const PrismSpec PRISMS[3] = {
    {6, {222, 0, -222, -222, 0, 222}, {128, 256, 128, -128, -256, -128}, 500, 300, 180, 780, 4, 120, 280},
    {3, {0, -222, 222}, {256, -128, -128}, 420, 260, 190, 700, 4, 120, 280},        /* Prism 3 */
    {6, {222, 0, -222, -222, 0, 222}, {128, 256, 128, -128, -256, -128}, 520, 300, 170, 780, 11, 120, 300},
};
typedef struct {
    const PrismSpec *spec;
    int w, h;
    uint8_t *k12;                  /* per pixel: its facet | the neighbour it blends with << 4 */
    uint8_t *g;                    /* how much of its own facet: 128 + g (the neighbour: 128 - g) */
    uint8_t *r;                    /* how much the facets show (0: the clear middle) */
    int dx[3][6], dy[3][6];        /* per colour and facet: how far the picture moves */
    unsigned used;
} PLayers;
static __thread PLayers players[2];                /* (per thread, like the masks) */
static __thread unsigned pclock;

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static const PLayers *prism_layers(const PrismSpec *P, int w, int h) {
    PLayers *slot = &players[0];
    for (int i = 0; i < 2; i++) {
        if (players[i].k12 && players[i].spec == P && players[i].w == w && players[i].h == h) {
            players[i].used = ++pclock;
            return &players[i];
        }
        if (!players[i].k12 || (slot->k12 && players[i].used < slot->used)) slot = &players[i];
    }
    free(slot->k12);
    free(slot->g);
    free(slot->r);
    size_t n = (size_t)w * h;
    PLayers L = {P, w, h, malloc(n), malloc(n), malloc(n), {{0}}, {{0}}, 0};
    int s = w < h ? w : h, nf = P->n;
    for (int y = 0; y < h; y++) {
        int yy = 2 * y + 1 - h;
        int v = (yy < 0 ? -1 : yy > 0) * (abs(yy) * 1024 / s);
        for (int x = 0; x < w; x++) {
            int xx = 2 * x + 1 - w;
            int u = (xx < 0 ? -1 : xx > 0) * (abs(xx) * 1024 / s);
            int d1 = u * P->nx[0] + v * P->ny[0], k1 = 0;
            for (int k = 1; k < nf; k++) {
                int d = u * P->nx[k] + v * P->ny[k];
                if (d > d1) { d1 = d; k1 = k; }
            }
            int kn = (k1 + 1) % nf, kp = (k1 + nf - 1) % nf;
            int dn = u * P->nx[kn] + v * P->ny[kn], dp = u * P->nx[kp] + v * P->ny[kp];
            int k2 = dn >= dp ? kn : kp, d2 = dn > dp ? dn : dp;
            int g = (d1 - d2) / P->soft;
            int rho = (d1 > 0 ? d1 : 0) >> 8;
            int r = (rho > P->clear ? rho - P->clear : 0) * 256 / P->ramp;
            size_t i = (size_t)y * w + x;
            L.k12[i] = (uint8_t)(k1 | k2 << 4);
            L.g[i] = (uint8_t)(g < 128 ? g : 128);
            L.r[i] = (uint8_t)(r < P->most ? r : P->most);
        }
    }
    for (int c = 0; c < 3; c++) {
        int dc = P->shift * (100 + (c - 1) * P->disp) / 100;
        for (int k = 0; k < nf; k++) {
            L.dx[c][k] = P->nx[k] * dc * s / (256 * 2048);    /* (C: rounded towards 0) */
            L.dy[c][k] = P->ny[k] * dc * s / (256 * 2048);
        }
    }
    L.used = ++pclock;
    *slot = L;
    return slot;
}

static inline uint8_t sat8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

static Img prism(int id, Img src) {
    const PrismSpec *P = &PRISMS[id == LOOK_PRISM_3 ? 1 : id == LOOK_PRISM_RAINBOW ? 2 : 0];
    Img in = src.c == 3 ? src : img_to_rgb(src);
    int w = in.w, h = in.h, nf = P->n, sat = P->sat;
    const PLayers *L = prism_layers(P, w, h);
    /* (v49) the picture with its edge pixels repeated left and right, so a
     * facet's moved pixel is one address away - no table, no clamping per
     * pixel (exactly the same pixels as clampi() on the column) */
    int pad = 0;
    for (int c = 0; c < 3; c++)
        for (int k = 0; k < nf; k++) {
            int d = abs(L->dx[c][k]);
            if (d > pad) pad = d;
        }
    Img pd = img_alloc(w + 2 * pad, h, 3);
    for (int y = 0; y < h; y++) {
        const uint8_t *s = PX(in, 0, y);
        uint8_t *d = PX(pd, 0, y);
        for (int x = 0; x < pad; x++) {
            memcpy(d + x * 3, s, 3);
            memcpy(d + (pad + w + x) * 3, s + (w - 1) * 3, 3);
        }
        memcpy(d + pad * 3, s, (size_t)w * 3);
    }
    Img o = img_alloc(w, h, 3);
    for (int y = 0; y < h; y++) {
        const uint8_t *fb[6][3];                     /* per facet and colour: [x * 3] is its pixel */
        for (int k = 0; k < nf; k++)
            for (int c = 0; c < 3; c++)
                fb[k][c] = PX(pd, pad - L->dx[c][k], clampi(y - L->dy[c][k], 0, h - 1)) + c;
        const uint8_t *row = PX(in, 0, y);
        uint8_t *q = PX(o, 0, y);
        size_t i0 = (size_t)y * w;
        const uint8_t *lr = L->r + i0, *lg = L->g + i0, *lk = L->k12 + i0;
        for (int x = 0, x3 = 0; x < w; x++, x3 += 3, q += 3) {
            int r = lr[x], v0, v1, v2;
            const uint8_t *p = row + x3;
            if (!r) {
                v0 = p[0], v1 = p[1], v2 = p[2];
            } else {
                int g = lg[x], k = lk[x], ir = 256 - r;
                const uint8_t *const *f1 = fb[k & 15];
                if (g == 128) {                      /* (inside one facet: (a * 256 + b * 0) >> 8 = a) */
                    v0 = (p[0] * ir + f1[0][x3] * r) >> 8;
                    v1 = (p[1] * ir + f1[1][x3] * r) >> 8;
                    v2 = (p[2] * ir + f1[2][x3] * r) >> 8;
                } else {
                    const uint8_t *const *f2 = fb[k >> 4];
                    int ga = 128 + g, gb = 128 - g;
                    v0 = (p[0] * ir + ((f1[0][x3] * ga + f2[0][x3] * gb) >> 8) * r) >> 8;
                    v1 = (p[1] * ir + ((f1[1][x3] * ga + f2[1][x3] * gb) >> 8) * r) >> 8;
                    v2 = (p[2] * ir + ((f1[2][x3] * ga + f2[2][x3] * gb) >> 8) * r) >> 8;
                }
            }
            int gr = (v0 * 77 + v1 * 150 + v2 * 29) >> 8;
            q[0] = sat8(gr + ((v0 - gr) * sat >> 8));
            q[1] = sat8(gr + ((v1 - gr) * sat >> 8));
            q[2] = sat8(gr + ((v2 - gr) * sat >> 8));
        }
    }
    img_free(&pd);
    if (in.p != src.p) img_free(&in);
    return o;
}

/* ---------------------------------------------- Zero Ichi (v52: colour film) */
/* filters.zeroichi_*: on the camera's own Y, U, V - the brightness through the
 * film curve (squeezed: room for the grain), grain, a gentle vignette and back;
 * the colours through the logo's table (by U >> 1, V >> 1), with less colour
 * at the ends and ink-blue shadows, cream highlights by the graded brightness */
#define ZI_GRAIN 9
#define ZI_VIG 0.22f

Img zi_luma(Img y) {
    int w = y.w, h = y.h;
    Img g = img_copy(y);
    img_point(g, T_ZI_YC);
    Img gr = grain_mod(w, h, ZI_GRAIN, 1);
    img_add_modulo(g, gr);
    img_free(&gr);
    img_multiply(g, vig_mask(w, h, ZI_VIG, 1, 1, 0));
    img_point(g, T_ZI_EXP);
    return g;
}

void zi_colour(Img T, Img U, Img V, Img *ou, Img *ov) {
    Img a = img_alloc(U.w, U.h, 1), b = img_alloc(U.w, U.h, 1);
    for (int y = 0; y < U.h; y++) {
        const uint8_t *t = PX(T, 0, y), *pu = PX(U, 0, y), *pv = PX(V, 0, y);
        uint8_t *qa = PX(a, 0, y), *qb = PX(b, 0, y);
        for (int x = 0; x < U.w; x++) {
            int k = t[x], s = ZI_SY[k], i = (pu[x] >> 1) << 7 | (pv[x] >> 1);
            int cu = ((ZI_CU[i] * s + 32896) >> 8) + ZI_TU[k];     /* (+ 32896: rounded, never < 0) */
            int cv = ((ZI_CV[i] * s + 32896) >> 8) + ZI_TV[k];
            qa[x] = (uint8_t)(cu < 0 ? 0 : cu > 255 ? 255 : cu);
            qb[x] = (uint8_t)(cv < 0 ? 0 : cv > 255 ? 255 : cv);
        }
    }
    *ou = a;
    *ov = b;
}

void zi_yuv(Img Y, Img U, Img V, Img *oy, Img *ou, Img *ov) {
    Img T = img_copy(Y);
    img_point(T, T_ZI_TONE);
    Img th = (T.w == 2 * U.w && T.h == 2 * U.h) ? img_reduce(T, 2) : img_resize(T, U.w, U.h, RS_NEAREST);
    img_free(&T);
    zi_colour(th, U, V, ou, ov);
    img_free(&th);
    *oy = zi_luma(Y);
}

/* (R,G,B pictures - GIFs: through Y, Cb, Cr at full size, as filters.zero_ichi) */
static Img zi_rgb(Img rgb) {
    Img in = rgb.c == 3 ? rgb : img_to_rgb(rgb);
    Img y, cb, cr, u, v;
    img_rgb_to_ycc(in, &y, &cb, &cr);
    if (in.p != rgb.p) img_free(&in);
    Img T = img_copy(y);
    img_point(T, T_ZI_TONE);
    zi_colour(T, cb, cr, &u, &v);
    img_free(&T);
    Img yo = zi_luma(y);
    Img o = img_ycc_to_rgb(yo, u, v);
    img_free(&y); img_free(&cb); img_free(&cr); img_free(&u); img_free(&v); img_free(&yo);
    return o;
}

/* ----------------------------------------------------------- any look */
Img look_rgb(int id, Img rgb, int ow, int oh) {
    switch (id) {
    case LOOK_8MM:
        return eightmm(rgb, ow, oh);
    case LOOK_HOLGA: case LOOK_GB: case LOOK_GB_BW: case LOOK_MONO: case LOOK_HICON: case LOOK_THERMAL: {
        Tone t = tone_make(id, rgb, 0, 0);
        Img o = tone_rgb(&t);
        tone_free(&t);
        return o;
    }
    case LOOK_PX_ARCADE: case LOOK_PX_NEON: case LOOK_PX_PAPER: {
        Tone t = pixel_tone_rgb(id, rgb, ow, oh);
        Img o = tone_rgb(&t);
        tone_free(&t);
        return o;
    }
    case LOOK_CROSS: {
        Img o = rgb.c == 3 ? img_copy(rgb) : img_to_rgb(rgb);
        img_point3(o, T_CROSS_R, T_CROSS_G, T_CROSS_B);
        return o;
    }
    case LOOK_HOLGA_COLOR:
        return holga_color(rgb);
    case LOOK_PRISM: case LOOK_PRISM_3: case LOOK_PRISM_RAINBOW:
        return prism(id, rgb);
    case LOOK_ZEROICHI:
        return zi_rgb(rgb);
    default:
        return rgb.c == 3 ? img_copy(rgb) : img_to_rgb(rgb);
    }
}

/* filters._yuv_rgb */
static Img yuv_rgb(Img Y, Img U, Img V, int w, int h) {
    /* (v50) the usual case - Y at the size, U and V enlarged to it: they are
     * enlarged a row at a time as the colours are worked out (the same
     * numbers, no full-size U, V planes to go through) */
    if (Y.w == w && Y.h == h && U.w < w && U.h < h) {
        ImgRows *ru = img_rows_begin(U, w, h), *rv = ru ? img_rows_begin(V, w, h) : NULL;
        if (rv) {
            Img o = img_alloc(w, h, 3);
            for (int y = 0; y < h; y++) {
                const uint8_t *a = PX(Y, 0, y), *b = img_rows_get(ru, y), *c = img_rows_get(rv, y);
                uint8_t *q = PX(o, 0, y);
                for (int x = 0; x < w; x++, q += 3) ycc2rgb(a[x], b[x], c[x], q);
            }
            img_rows_end(ru);
            img_rows_end(rv);
            return o;
        }
        img_rows_end(ru);
    }
    Img p[3] = {Y, U, V}, r[3];
    for (int i = 0; i < 3; i++)
        r[i] = (p[i].w == w && p[i].h == h) ? img_copy(p[i]) : img_resize_gap(p[i], w, h, RS_BILINEAR, 2.0);
    Img o = img_ycc_to_rgb(r[0], r[1], r[2]);
    for (int i = 0; i < 3; i++) img_free(&r[i]);
    return o;
}

int look_photo(int id, Img Y, Img U, Img V, Img *oy, Img *ou, Img *ov, Img *rgb) {
    int w = Y.w, h = Y.h;
    if (id == LOOK_NORMAL) {
        *oy = img_copy(Y); *ou = img_copy(U); *ov = img_copy(V);
        return 1;
    }
    if (look_is_tone(id)) {
        Tone t = tone_make(id, Y, w, h);
        tone_yuv(&t, 0, 0, 0, oy, ou, ov);
        tone_free(&t);
        return 1;
    }
    if (id == LOOK_8MM) {                          /* (0.1.0: at half size - U, V's own) */
        int sw = w / 2, sh = pyround((double)h * sw / w);
        Img small = yuv_rgb(Y, U, V, sw, sh < 1 ? 1 : sh);
        *rgb = eightmm(small, w, h);
        img_free(&small);
        return 0;
    }
    if (look_is_pixel(id)) {
        Tone t = pixel_tone_planes(id, Y, U, V, w, h);
        tone_yuv(&t, 0, 0, 0, oy, ou, ov);
        tone_free(&t);
        return 1;
    }
    if (id == LOOK_ZEROICHI) {                          /* (no R,G,B at all) */
        zi_yuv(Y, U, V, oy, ou, ov);
        return 1;
    }
    if (id == LOOK_HOLGA_COLOR) {                       /* (v55: no R,G,B either) */
        hc_yuv(Y, U, V, oy, ou, ov);
        return 1;
    }
    Img full = yuv_rgb(Y, U, V, w, h);                  /* Cross, Prism */
    if (id == LOOK_CROSS) {                             /* (its tables: straight on the picture) */
        img_point3(full, T_CROSS_R, T_CROSS_G, T_CROSS_B);
        *rgb = full;
        return 0;
    }
    *rgb = look_rgb(id, full, 0, 0);
    img_free(&full);
    return 0;
}

/* ----------------------------------------------------- 16-bit screen */
static uint16_t *yuv565[NLOOKS];
static uint16_t pal565[16][256];
static const Palette *pal565_of[16];
static pthread_mutex_t t565_lock = PTHREAD_MUTEX_INITIALIZER;

static inline uint16_t rgb565(int r, int g, int b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

const uint16_t *yuv565_table(int id) {
    pthread_mutex_lock(&t565_lock);
    if (!yuv565[id]) {
        uint16_t *t = malloc(65536 * 2);
        for (int yi = 0; yi < 64; yi++)
            for (int ui = 0; ui < 32; ui++)
                for (int vi = 0; vi < 32; vi++) {
                    float Y = yi * 4 + 2.0f, U = ui * 8 + 4.0f - 128.0f, V = vi * 8 + 4.0f - 128.0f;
                    if (id == LOOK_ZEROICHI) {          /* (its colours: the screen's brightness as T) */
                        int k = yi * 4 + 2, s = ZI_SY[k], i = ((ui * 8 + 4) >> 1) << 7 | ((vi * 8 + 4) >> 1);
                        int cu = ((ZI_CU[i] * s + 32896) >> 8) + ZI_TU[k];
                        int cv = ((ZI_CV[i] * s + 32896) >> 8) + ZI_TV[k];
                        U = (float)((cu < 0 ? 0 : cu > 255 ? 255 : cu) - 128);
                        V = (float)((cv < 0 ? 0 : cv > 255 ? 255 : cv) - 128);
                    }
                    volatile float r = Y + 1.402f * V;
                    volatile float g1 = 0.344136f * U, g2 = 0.714136f * V;
                    volatile float g = Y - g1 - g2;
                    volatile float b = Y + 1.772f * U;
                    float c[3] = {r + 0.5f, g + 0.5f, b + 0.5f};
                    int v[3];
                    for (int i = 0; i < 3; i++) v[i] = c[i] <= 0 ? 0 : c[i] >= 255 ? 255 : (int)c[i];
                    if (id == LOOK_CROSS) {
                        v[0] = T_CROSS_R[v[0]]; v[1] = T_CROSS_G[v[1]]; v[2] = T_CROSS_B[v[2]];
                    }
                    t[yi << 10 | ui << 5 | vi] = rgb565(v[0], v[1], v[2]);
                }
        yuv565[id] = t;
    }
    pthread_mutex_unlock(&t565_lock);
    return yuv565[id];
}

const uint16_t *palette_565(const Palette *p) {
    if (!p) p = &PAL_GRAY;
    pthread_mutex_lock(&t565_lock);
    int i;
    for (i = 0; i < 16 && pal565_of[i]; i++)
        if (pal565_of[i] == p) break;
    if (i == 16) i = 15;
    if (pal565_of[i] != p) {
        for (int k = 0; k < 256; k++) pal565[i][k] = rgb565(p->r[k], p->g[k], p->b[k]);
        pal565_of[i] = p;
    }
    pthread_mutex_unlock(&t565_lock);
    return pal565[i];
}

void looks_init(void) {
    const Palette *pals[3] = {&PAL_PIXEL_0, &PAL_PIXEL_1, &PAL_PIXEL_2};
    const uint8_t *luts[3] = {LUT_PIXEL_0, LUT_PIXEL_1, LUT_PIXEL_2};
    int amps[3] = {AMP_PIXEL_0, AMP_PIXEL_1, AMP_PIXEL_2};
    for (int i = 0; i < 3; i++) {
        PixelPal *pp = &pixpal[i];
        pp->pal = pals[i];
        pp->lut = luts[i];
        pp->amp = amps[i];
        for (int v = 0; v < 256 + 2 * pp->amp && v < 256 + 64; v++) {
            int c = v - pp->amp;
            c = c < 0 ? 0 : c > 255 ? 255 : c;
            pp->ty[v] = (uint16_t)((c >> 3) << 10);
        }
        for (int q = 0; q < 256; q++) {
            pp->tu[q] = (uint16_t)((q >> 3) << 5);
            pp->tv[q] = (uint16_t)(q >> 3);
        }
    }
}
