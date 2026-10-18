/* Zero Ichi - picture operations, following Pillow's libImaging (see img.h). */
#include "img.h"
#include <pthread.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "ycc_inline.h"

/* Picture memory: big buffers come back for the next picture of the same
 * size (the live view and videos make the same sizes over and over) instead
 * of going back to the system - which hands them out again as fresh pages,
 * one page fault each. */
#define POOL_N 16
#define POOL_MIN (32 * 1024)
#define POOL_MAX (1100 * 1024)
static struct { void *p; size_t n; } pool[POOL_N];
static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;

static void *buf_get(size_t n, int zero) {
    if (n >= POOL_MIN && n <= POOL_MAX) {
        void *p = NULL;
        pthread_mutex_lock(&pool_lock);
        for (int i = 0; i < POOL_N; i++)
            if (pool[i].p && pool[i].n == n) {
                p = pool[i].p;
                pool[i].p = NULL;
                break;
            }
        pthread_mutex_unlock(&pool_lock);
        if (p) {
            if (zero) memset(p, 0, n);
            return p;
        }
    }
    return zero ? calloc(n, 1) : malloc(n);
}

static void buf_put(void *p, size_t n) {
    if (p && n >= POOL_MIN && n <= POOL_MAX) {
        pthread_mutex_lock(&pool_lock);
        for (int i = 0; i < POOL_N; i++)
            if (!pool[i].p) {
                pool[i].p = p;
                pool[i].n = n;
                p = NULL;
                break;
            }
        pthread_mutex_unlock(&pool_lock);
    }
    free(p);
}

static Img img_make(int w, int h, int c, int zero) {
    Img a = {w, h, c, w * c, NULL, 1};
    if (w < 1) a.w = w = 1;
    if (h < 1) a.h = h = 1;
    a.stride = w * c;
    a.p = buf_get((size_t)a.stride * h + 16, zero);
    if (!a.p) abort();
    return a;
}

Img img_new(int w, int h, int c) { return img_make(w, h, c, 1); }

/* (every pixel will be written: no need to clear it first) */
Img img_alloc(int w, int h, int c) { return img_make(w, h, c, 0); }

Img img_view(uint8_t *p, int w, int h, int c, int stride) {
    Img a = {w, h, c, stride, p, 0};
    return a;
}

Img img_sub(Img a, int x, int y, int w, int h) {
    Img s = a;
    s.p = PX(a, x, y);
    s.w = w;
    s.h = h;
    s.own = 0;
    return s;
}

Img img_copy(Img a) {
    Img b = img_alloc(a.w, a.h, a.c);
    for (int y = 0; y < a.h; y++) memcpy(PX(b, 0, y), PX(a, 0, y), (size_t)a.w * a.c);
    return b;
}

void img_free(Img *a) {
    if (a->own) buf_put(a->p, (size_t)a->stride * a->h + 16);
    a->p = NULL;
    a->own = 0;
}

void img_fill(Img a, int r, int g, int b) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        if (a.c == 1) {
            memset(p, r, a.w);
        } else {
            for (int x = 0; x < a.w; x++, p += 3) { p[0] = r; p[1] = g; p[2] = b; }
        }
    }
}

/* ---------------------------------------------------------------- resample */
#define PRECISION_BITS (32 - 8 - 2)

static double filt_bilinear(double x) {
    if (x < 0.0) x = -x;
    return x < 1.0 ? 1.0 - x : 0.0;
}
static double filt_box(double x) {
    return (x > -0.5 && x <= 0.5) ? 1.0 : 0.0;
}

/* Pillow's precompute_coeffs + normalize_coeffs_8bpc */
static int coeffs(int in_size, float in0, float in1, int out_size, int filter,
                  int **boundsp, int32_t **kkp) {
    double (*fn)(double) = filter == RS_BOX ? filt_box : filt_bilinear;
    double support0 = filter == RS_BOX ? 0.5 : 1.0;
    double scale, filterscale;
    filterscale = scale = (double)(in1 - in0) / out_size;
    if (filterscale < 1.0) filterscale = 1.0;
    double support = support0 * filterscale;
    int ksize = (int)ceil(support) * 2 + 1;
    double *kk = malloc(sizeof(double) * out_size * ksize);
    int *bounds = malloc(sizeof(int) * out_size * 2);
    int32_t *ik = malloc(sizeof(int32_t) * out_size * ksize);
    for (int xx = 0; xx < out_size; xx++) {
        double center = in0 + (xx + 0.5) * scale;
        double ww = 0.0, ss = 1.0 / filterscale;
        int xmin = (int)(center - support + 0.5);
        if (xmin < 0) xmin = 0;
        int xmax = (int)(center + support + 0.5);
        if (xmax > in_size) xmax = in_size;
        xmax -= xmin;
        double *k = &kk[xx * ksize];
        int x;
        for (x = 0; x < xmax; x++) {
            double w = fn((x + xmin - center + 0.5) * ss);
            k[x] = w;
            ww += w;
        }
        for (x = 0; x < xmax; x++)
            if (ww != 0.0) k[x] /= ww;
        for (; x < ksize; x++) k[x] = 0;
        bounds[xx * 2] = xmin;
        bounds[xx * 2 + 1] = xmax;
    }
    for (int i = 0; i < out_size * ksize; i++)
        ik[i] = kk[i] < 0 ? (int)(-0.5 + kk[i] * (1 << PRECISION_BITS))
                          : (int)(0.5 + kk[i] * (1 << PRECISION_BITS));
    free(kk);
    *boundsp = bounds;
    *kkp = ik;
    return ksize;
}

static inline uint8_t clip8i(int v) {
    v >>= PRECISION_BITS;
    return v < 0 ? 0 : v > 255 ? 255 : v;
}

/* Enlarging (bilinear, scale 1 or more): of Pillow's weights for an output
 * pixel at most two are not 0, so the same sums are made with just those two
 * (exactly the same numbers, a lot quicker - e.g. the video frames enlarged
 * from the screen's size). Both weights >= 0 and together at most 1 (plus
 * rounding): no clipping needed. NULL: not this kind of resize. */
typedef struct { int i0, i1; int32_t k0, k1; } Tap2;
#define HALF_P (1 << (PRECISION_BITS - 1))

static Tap2 *taps2(int out_size, const int *bounds, const int32_t *kk, int ksize) {
    if (ksize != 3) return NULL;                     /* (support 1: bilinear, not shrinking) */
    Tap2 *t = malloc(sizeof(Tap2) * (out_size > 0 ? out_size : 1));
    for (int x = 0; x < out_size; x++) {
        int xmin = bounds[x * 2], xmax = bounds[x * 2 + 1], n = 0, idx[2] = {xmin, xmin};
        int32_t k[2] = {0, 0};
        const int32_t *w = kk + (size_t)x * ksize;
        for (int j = 0; j < xmax; j++) {
            if (!w[j]) continue;
            if (n == 2) { free(t); return NULL; }
            idx[n] = xmin + j;
            k[n++] = w[j];
        }
        if (n == 1) idx[1] = idx[0];
        if (k[0] < 0 || k[1] < 0 || (int64_t)k[0] + k[1] > (1 << PRECISION_BITS) + 2) { free(t); return NULL; }
        t[x] = (Tap2){idx[0], idx[1], k[0], k[1]};
    }
    return t;
}

/* (into: a picture of w x h - e.g. a view into the video encoder's buffer -
 * that gets the result, instead of a new one) */
static Img resample(Img a, int w, int h, int filter, const float box[4], const Img *into) {
    int need_h = w != a.w || box[0] != 0 || box[2] != w;
    int need_v = h != a.h || box[1] != 0 || box[3] != h;
    int *bh, *bv;
    int32_t *kh, *kv;
    int ksh = coeffs(a.w, box[0], box[2], w, filter, &bh, &kh);
    int ksv = coeffs(a.h, box[1], box[3], h, filter, &bv, &kv);
    int yfirst = bv[0];
    int ylast = bv[h * 2 - 2] + bv[h * 2 - 1];
    int c = a.c;
    Img src = a, tmp = {0}, out = {0};
    if (need_h) {
        for (int i = 0; i < h; i++) bv[i * 2] -= yfirst;
        tmp = (!need_v && into) ? *into : img_alloc(w, ylast - yfirst, c);
        Tap2 *t2 = taps2(w, bh, kh, ksh);
        if (t2) {                                            /* enlarging: two weights */
            for (int yy = 0; yy < tmp.h; yy++) {
                const uint8_t *in = PX(a, 0, yy + yfirst);
                uint8_t *o = PX(tmp, 0, yy);
                if (c == 1) {
                    for (int xx = 0; xx < w; xx++) {
                        const Tap2 *t = &t2[xx];
                        o[xx] = (uint8_t)((HALF_P + in[t->i0] * t->k0 + in[t->i1] * t->k1) >> PRECISION_BITS);
                    }
                } else if (c == 3) {
                    for (int xx = 0; xx < w; xx++, o += 3) {
                        const Tap2 *t = &t2[xx];
                        const uint8_t *p0 = in + t->i0 * 3, *p1 = in + t->i1 * 3;
                        int32_t k0 = t->k0, k1 = t->k1;
                        o[0] = (uint8_t)((HALF_P + p0[0] * k0 + p1[0] * k1) >> PRECISION_BITS);
                        o[1] = (uint8_t)((HALF_P + p0[1] * k0 + p1[1] * k1) >> PRECISION_BITS);
                        o[2] = (uint8_t)((HALF_P + p0[2] * k0 + p1[2] * k1) >> PRECISION_BITS);
                    }
                } else {
                    for (int xx = 0; xx < w; xx++, o += c) {
                        const Tap2 *t = &t2[xx];
                        const uint8_t *p0 = in + t->i0 * c, *p1 = in + t->i1 * c;
                        for (int ch = 0; ch < c; ch++)
                            o[ch] = (uint8_t)((HALF_P + p0[ch] * t->k0 + p1[ch] * t->k1) >> PRECISION_BITS);
                    }
                }
            }
            free(t2);
            goto vertical;
        }
        for (int yy = 0; yy < tmp.h; yy++) {
            const uint8_t *in = PX(a, 0, yy + yfirst);
            uint8_t *o = PX(tmp, 0, yy);
            for (int xx = 0; xx < w; xx++) {
                int xmin = bh[xx * 2], xmax = bh[xx * 2 + 1];
                const int32_t *k = &kh[xx * ksh];
                if (c == 1) {                                  /* (the usual: one plane) */
                    const uint8_t *p = in + xmin;
                    int ss = 1 << (PRECISION_BITS - 1);
                    for (int x = 0; x < xmax; x++) ss += p[x] * k[x];
                    o[xx] = clip8i(ss);
                    continue;
                }
                if (c == 3) {                                  /* (R,G,B: the three sums at once) */
                    const uint8_t *p = in + xmin * 3;
                    int s0 = HALF_P, s1 = HALF_P, s2 = HALF_P;
                    for (int x = 0; x < xmax; x++, p += 3) {
                        int32_t kx = k[x];
                        s0 += p[0] * kx;
                        s1 += p[1] * kx;
                        s2 += p[2] * kx;
                    }
                    o[xx * 3] = clip8i(s0);
                    o[xx * 3 + 1] = clip8i(s1);
                    o[xx * 3 + 2] = clip8i(s2);
                    continue;
                }
                for (int ch = 0; ch < c; ch++) {
                    int ss = 1 << (PRECISION_BITS - 1);
                    for (int x = 0; x < xmax; x++) ss += in[(x + xmin) * c + ch] * k[x];
                    o[xx * c + ch] = clip8i(ss);
                }
            }
        }
    }
vertical:
    if (need_h) src = tmp;
    if (need_v) {
        out = into ? *into : img_alloc(w, h, c);
        Tap2 *t2 = taps2(h, bv, kv, ksv);
        if (t2) {                                            /* enlarging: two rows */
            size_t n = (size_t)w * c;
            for (int yy = 0; yy < h; yy++) {
                const uint8_t *r0 = PX(src, 0, t2[yy].i0), *r1 = PX(src, 0, t2[yy].i1);
                int32_t k0 = t2[yy].k0, k1 = t2[yy].k1;
                uint8_t *o = PX(out, 0, yy);
                if (!k1) {
                    if (k0 == (1 << PRECISION_BITS)) { memcpy(o, r0, n); continue; }
                    for (size_t i = 0; i < n; i++) o[i] = (uint8_t)((HALF_P + r0[i] * k0) >> PRECISION_BITS);
                    continue;
                }
                for (size_t i = 0; i < n; i++)
                    o[i] = (uint8_t)((HALF_P + r0[i] * k0 + r1[i] * k1) >> PRECISION_BITS);
            }
            free(t2);
            if (need_h) img_free(&tmp);
            free(bh); free(bv); free(kh); free(kv);
            return out;
        }
        const uint8_t **rp = malloc(sizeof(uint8_t *) * (ksv > 0 ? ksv : 1));
        for (int yy = 0; yy < h; yy++) {
            int ymin = bv[yy * 2], ymax = bv[yy * 2 + 1];
            const int32_t *k = &kv[yy * ksv];
            uint8_t *o = PX(out, 0, yy);
            for (int y = 0; y < ymax; y++) rp[y] = PX(src, 0, y + ymin);   /* (the rows, once) */
            for (int i = 0; i < w * c; i++) {
                int ss = 1 << (PRECISION_BITS - 1);
                for (int y = 0; y < ymax; y++) ss += rp[y][i] * k[y];
                o[i] = clip8i(ss);
            }
        }
        free(rp);
        if (need_h) img_free(&tmp);
    } else if (need_h) {
        out = tmp;
    } else if (into) {
        out = *into;
        for (int yy = 0; yy < h; yy++) memcpy(PX(out, 0, yy), PX(a, 0, yy), (size_t)w * c);
    } else {
        out = img_copy(a);
    }
    free(bh); free(bv); free(kh); free(kv);
    return out;
}

/* (v50) Enlarging, one output row at a time - the same numbers as
 * img_resize(a, w, h, RS_BILINEAR), for a pass that uses each enlarged row
 * once (no whole enlarged picture is made: less memory to go through). */
struct ImgRows {
    Img src, tmp;                  /* tmp: the rows enlarged across (or src itself) */
    Tap2 *tv;                      /* the rows down (NULL: the same height) */
    uint8_t *row;
    int w, h, c, two;              /* two: exactly twice the size (resize2x_bilinear's sums) */
    int ready;                     /* (two: the even row made with the odd one before it) */
};

static void double_row(const uint8_t *s, uint8_t *d, int w, int c);

ImgRows *img_rows_begin(Img a, int w, int h) {
    if (w < a.w || h < a.h || w < 1 || h < 1) return NULL;
    if (w == 2 * a.w && h == 2 * a.h) {              /* (twice: whole-number weights, quicker) */
        ImgRows *e = calloc(1, sizeof *e);
        e->src = a; e->w = w; e->h = h; e->c = a.c; e->two = 1;
        e->tmp = img_alloc(w, a.h, a.c);
        for (int y = 0; y < a.h; y++) double_row(PX(a, 0, y), PX(e->tmp, 0, y), a.w, a.c);
        e->row = malloc(2 * ((size_t)w * a.c + 16));
        e->ready = -1;
        return e;
    }
    int *bh, *bv;
    int32_t *kh, *kv;
    int ksh = coeffs(a.w, 0, (float)a.w, w, RS_BILINEAR, &bh, &kh);
    int ksv = coeffs(a.h, 0, (float)a.h, h, RS_BILINEAR, &bv, &kv);
    int need_h = w != a.w, need_v = h != a.h, c = a.c;
    int yfirst = bv[0], ylast = bv[h * 2 - 2] + bv[h * 2 - 1];
    ImgRows *e = calloc(1, sizeof *e);
    e->src = a; e->w = w; e->h = h; e->c = c;
    Tap2 *th = NULL;
    int ok = 1;
    if (need_h) {
        th = taps2(w, bh, kh, ksh);
        if (!th) ok = 0;
        else for (int i = 0; i < h; i++) bv[i * 2] -= yfirst;
    }
    if (ok && need_v && !(e->tv = taps2(h, bv, kv, ksv))) ok = 0;
    if (ok && need_h) {                              /* (resample's pass across, as it is) */
        e->tmp = img_alloc(w, ylast - yfirst, c);
        for (int yy = 0; yy < e->tmp.h; yy++) {
            const uint8_t *in = PX(a, 0, yy + yfirst);
            uint8_t *o = PX(e->tmp, 0, yy);
            if (c == 1) {
                for (int xx = 0; xx < w; xx++) {
                    const Tap2 *t = &th[xx];
                    o[xx] = (uint8_t)((HALF_P + in[t->i0] * t->k0 + in[t->i1] * t->k1) >> PRECISION_BITS);
                }
            } else {
                for (int xx = 0; xx < w; xx++, o += c) {
                    const Tap2 *t = &th[xx];
                    const uint8_t *p0 = in + t->i0 * c, *p1 = in + t->i1 * c;
                    for (int ch = 0; ch < c; ch++)
                        o[ch] = (uint8_t)((HALF_P + p0[ch] * t->k0 + p1[ch] * t->k1) >> PRECISION_BITS);
                }
            }
        }
    } else if (ok) {
        e->tmp = a;
        e->tmp.own = 0;
    }
    free(th); free(bh); free(bv); free(kh); free(kv);
    if (!ok) {
        free(e->tv);
        free(e);
        return NULL;
    }
    e->row = malloc((size_t)w * c + 16);
    return e;
}

const uint8_t *img_rows_get(ImgRows *e, int y) {
    if (e->two) {                                    /* (resize2x_bilinear's rows) */
        if (y == 0) return PX(e->tmp, 0, 0);
        if (y == e->h - 1) return PX(e->tmp, 0, e->tmp.h - 1);
        int i = (y + 1) / 2;
        const uint8_t *r0 = PX(e->tmp, 0, i - 1), *r1 = PX(e->tmp, 0, i);
        size_t n = (size_t)e->w * e->c;
        uint8_t *od = e->row, *ev = e->row + n + 16;
        if (!(y & 1) && e->ready == y) return ev;    /* (made already, with the row above) */
        if (y & 1) {                                 /* rows 2i-1 and 2i: from the same two rows */
            for (size_t x = 0; x < n; x++) {
                int p0 = r0[x], p1 = r1[x];
                od[x] = (uint8_t)((3 * p0 + p1 + 2) >> 2);
                ev[x] = (uint8_t)((p0 + 3 * p1 + 2) >> 2);
            }
            e->ready = y + 1;
            return od;
        }
        for (size_t x = 0; x < n; x++) ev[x] = (uint8_t)((r0[x] + 3 * r1[x] + 2) >> 2);
        e->ready = -1;
        return ev;
    }
    if (!e->tv) return PX(e->tmp, 0, y);
    const Tap2 *t = &e->tv[y];
    const uint8_t *r0 = PX(e->tmp, 0, t->i0), *r1 = PX(e->tmp, 0, t->i1);
    int32_t k0 = t->k0, k1 = t->k1;
    if (!k1 && k0 == (1 << PRECISION_BITS)) return r0;
    uint8_t *o = e->row;
    size_t n = (size_t)e->w * e->c;
    for (size_t i = 0; i < n; i++) o[i] = (uint8_t)((HALF_P + r0[i] * k0 + r1[i] * k1) >> PRECISION_BITS);
    return o;
}

void img_rows_end(ImgRows *e) {
    if (!e) return;
    if (e->tmp.own) img_free(&e->tmp);
    free(e->tv);
    free(e->row);
    free(e);
}

static Img resize_nearest(Img a, int w, int h, const Img *into) {
    /* Pillow's ImagingScaleAffine: positions added up in floating point (a
     * source pixel just outside the picture leaves black, like Pillow) */
    double a0 = (double)a.w / w, a4 = (double)a.h / h;
    Img o = into ? *into : img_new(w, h, a.c);       /* (zero-filled) */
    int *xs = malloc(sizeof(int) * w);
    double xo = a0 * 0.5, yo = a4 * 0.5;
    for (int x = 0; x < w; x++, xo += a0) {
        int xin = xo < 0.0 ? -1 : (int)xo;
        xs[x] = (xin >= 0 && xin < a.w) ? xin : -1;
    }
    int x0 = 0, x1 = w;                              /* (the columns that have a source) */
    while (x0 < w && xs[x0] < 0) x0++;
    while (x1 > x0 && xs[x1 - 1] < 0) x1--;
    int prev = -1;
    uint8_t *prev_row = NULL;
    for (int y = 0; y < h; y++, yo += a4) {
        int yin = yo < 0.0 ? -1 : (int)yo;
        if (yin < 0 || yin >= a.h) {
            if (into) memset(PX(o, 0, y), 0, (size_t)w * a.c);
            continue;
        }
        uint8_t *o2 = PX(o, 0, y);
        if (into) {                                  /* (no source: black, like a new picture) */
            memset(o2, 0, (size_t)x0 * a.c);
            memset(o2 + (size_t)x1 * a.c, 0, (size_t)(w - x1) * a.c);
        }
        if (yin == prev) {                           /* enlarging: the same row again */
            memcpy(o2, prev_row, (size_t)w * a.c);
            continue;
        }
        const uint8_t *in = PX(a, 0, yin);
        if (a.c == 1) {
            for (int x = x0; x < x1; x++)
                if (xs[x] >= 0) o2[x] = in[xs[x]];
        } else {
            for (int x = x0; x < x1; x++) {
                if (xs[x] < 0) continue;
                const uint8_t *s3 = in + xs[x] * 3;
                uint8_t *d3 = o2 + x * 3;
                d3[0] = s3[0];
                d3[1] = s3[1];
                d3[2] = s3[2];
            }
        }
        prev = yin;
        prev_row = o2;
    }
    free(xs);
    return o;
}

/* Exactly twice the size, bilinear: what resample() gives (Pillow's weights
 * are 1/4 and 3/4 then - whole numbers), much quicker. Rows first, then
 * columns, rounded in between, like Pillow. */
/* one row, twice as wide (Pillow's 1/4 - 3/4 weights, edges copied) */
static void double_row(const uint8_t *s, uint8_t *d, int w, int c) {
    int W = 2 * w;
    for (int k = 0; k < c; k++) {
        d[k] = s[k];                                           /* x = 0 */
        d[(W - 1) * c + k] = s[(w - 1) * c + k];               /* x = 2w-1 */
    }
    if (c == 1) {                                              /* (the usual: Y planes) */
        for (int i = 1; i < w; i++) {
            int p0 = s[i - 1], p1 = s[i];
            d[2 * i] = (uint8_t)((p0 + 3 * p1 + 2) >> 2);
            d[2 * i - 1] = (uint8_t)((3 * p0 + p1 + 2) >> 2);
        }
    } else {
        for (int i = 1; i < w; i++)
            for (int k = 0; k < c; k++) {
                int p0 = s[(i - 1) * c + k], p1 = s[i * c + k];
                d[(2 * i) * c + k] = (uint8_t)((p0 + 3 * p1 + 2) >> 2);
                d[(2 * i - 1) * c + k] = (uint8_t)((3 * p0 + p1 + 2) >> 2);
            }
    }
}

static Img resize2x_bilinear(Img a, const Img *into) {
    int w = a.w, h = a.h, c = a.c, W = 2 * w, H = 2 * h;
    Img t = img_alloc(W, h, c), o = into ? *into : img_alloc(W, H, c);
    for (int y = 0; y < h; y++) double_row(PX(a, 0, y), PX(t, 0, y), w, c);
    size_t n = (size_t)W * c;
    memcpy(PX(o, 0, 0), PX(t, 0, 0), n);
    memcpy(PX(o, 0, H - 1), PX(t, 0, h - 1), n);
    for (int i = 1; i < h; i++) {
        const uint8_t *r0 = PX(t, 0, i - 1), *r1 = PX(t, 0, i);
        uint8_t *e = PX(o, 0, 2 * i), *od = PX(o, 0, 2 * i - 1);
        for (size_t x = 0; x < n; x++) {
            int p0 = r0[x], p1 = r1[x];
            e[x] = (uint8_t)((p0 + 3 * p1 + 2) >> 2);
            od[x] = (uint8_t)((3 * p0 + p1 + 2) >> 2);
        }
    }
    img_free(&t);
    return o;
}

static Img resize_to(Img a, int w, int h, int filter, const Img *into) {
    if (filter == RS_NEAREST) return resize_nearest(a, w, h, into);
    if (filter == RS_BILINEAR && w == 2 * a.w && h == 2 * a.h && a.w >= 1 && a.h >= 1) return resize2x_bilinear(a, into);
    float box[4] = {0, 0, (float)a.w, (float)a.h};
    return resample(a, w, h, filter, box, into);
}

Img img_resize(Img a, int w, int h, int filter) {
    if (w == a.w && h == a.h) return img_copy(a);
    return resize_to(a, w, h, filter, NULL);
}

/* the same, into dst (its size; e.g. a view into the video encoder's buffer) */
void img_resize_into(Img a, Img dst, int filter) {
    if (dst.w == a.w && dst.h == a.h) {
        for (int y = 0; y < a.h; y++) memcpy(PX(dst, 0, y), PX(a, 0, y), (size_t)a.w * a.c);
        return;
    }
    resize_to(a, dst.w, dst.h, filter, &dst);
}

/* ------------------------------------------------------------------ reduce */
static uint32_t div_u32(int divider, int result_bits) {
    uint32_t max_dividend = (1u << result_bits) * divider;
    float max_int = (1 << 30) * 4.0f;
    return (uint32_t)(max_int / max_dividend);
}

static void reduce_box(Img a, Img o, int bx, int by, int bw, int bh, int fx, int fy) {
    int c = a.c;
    for (int y = 0; y < o.h; y++) {
        int y0 = by + y * fy, y1 = y0 + fy;
        if (y1 > by + bh) y1 = by + bh;
        /* (v50) the whole blocks of this row: the same sums, the block's
         * divisor worked out once and the three colours added up together */
        int full = bw / fx < o.w ? bw / fx : o.w, ny = y1 - y0, start = 0;
        if (full > 0 && ny > 0 && (c == 1 || c == 3)) {
            int n = fx * ny;
            uint32_t mul = div_u32(n, 8), amend = (uint32_t)n / 2;
            uint8_t *q = PX(o, 0, y);
            if (c == 3) {
                for (int x = 0; x < full; x++, q += 3) {
                    uint32_t s0 = amend, s1 = amend, s2 = amend;
                    for (int yy = y0; yy < y1; yy++) {
                        const uint8_t *p = PX(a, bx + x * fx, yy);
                        for (int k = 0; k < fx; k++, p += 3) {
                            s0 += p[0];
                            s1 += p[1];
                            s2 += p[2];
                        }
                    }
                    q[0] = (uint8_t)((s0 * mul) >> 24);
                    q[1] = (uint8_t)((s1 * mul) >> 24);
                    q[2] = (uint8_t)((s2 * mul) >> 24);
                }
            } else if (fx == 2 && ny == 2) {         /* (v55: 2 x 2 - Holga Color's soft picture) */
                const uint8_t *r0 = PX(a, bx, y0), *r1 = PX(a, bx, y0 + 1);
                for (int x = 0; x < full; x++, r0 += 2, r1 += 2)
                    q[x] = (uint8_t)(((amend + r0[0] + r0[1] + r1[0] + r1[1]) * mul) >> 24);
            } else {
                for (int x = 0; x < full; x++) {
                    uint32_t ss = amend;
                    for (int yy = y0; yy < y1; yy++) {
                        const uint8_t *p = PX(a, bx + x * fx, yy);
                        for (int k = 0; k < fx; k++) ss += p[k];
                    }
                    q[x] = (uint8_t)((ss * mul) >> 24);
                }
            }
            start = full;
        }
        for (int x = start; x < o.w; x++) {
            int x0 = bx + x * fx, x1 = x0 + fx;
            if (x1 > bx + bw) x1 = bx + bw;
            int n = (x1 - x0) * (y1 - y0);
            uint32_t mul = div_u32(n, 8), amend = n / 2;
            for (int ch = 0; ch < c; ch++) {
                uint32_t ss = amend;
                for (int yy = y0; yy < y1; yy++) {
                    const uint8_t *p = PX(a, x0, yy) + ch;
                    for (int xx = x0; xx < x1; xx++, p += c) ss += *p;
                }
                PX(o, x, y)[ch] = (uint8_t)((ss * mul) >> 24);
            }
        }
    }
}

Img img_reduce2(Img a, int fx, int fy) {
    if (fx <= 1 && fy <= 1) return img_copy(a);
    Img o = img_new((a.w + fx - 1) / fx, (a.h + fy - 1) / fy, a.c);
    reduce_box(a, o, 0, 0, a.w, a.h, fx, fy);
    return o;
}

Img img_reduce(Img a, int f) { return img_reduce2(a, f, f); }

Img img_resize_gap(Img a, int w, int h, int filter, double gap) {
    if (filter == RS_NEAREST || gap <= 0) return img_resize(a, w, h, filter);
    int fx = (int)((double)a.w / w / gap), fy = (int)((double)a.h / h / gap);
    if (fx < 1) fx = 1;
    if (fy < 1) fy = 1;
    if (fx == 1 && fy == 1) return img_resize(a, w, h, filter);
    /* (the whole picture: Pillow's "safe box" is all of it) */
    Img r = img_reduce2(a, fx, fy);
    float box[4] = {0, 0, (float)a.w / fx, (float)a.h / fy};
    Img o;
    if (w == r.w && h == r.h && box[2] == w && box[3] == h)
        o = img_copy(r);
    else
        o = resample(r, w, h, filter, box, NULL);
    img_free(&r);
    return o;
}

Img img_shrink_fast(Img a, int gw, int gh) {
    int f = a.w / gw < a.h / gh ? a.w / gw : a.h / gh;
    Img cur = a, red = {0};
    if (f >= 2) {
        red = img_reduce(a, f);
        cur = red;
    }
    Img o = (cur.w == gw && cur.h == gh) ? img_copy(cur) : img_resize(cur, gw, gh, RS_BILINEAR);
    if (red.p) img_free(&red);
    return o;
}

/* ---------------------------------------------------------- point & chops */
void img_point(Img a, const uint8_t *lut) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        for (int i = 0; i < a.w * a.c; i++) p[i] = lut[p[i]];
    }
}

void img_point3(Img a, const uint8_t *lr, const uint8_t *lg, const uint8_t *lb) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        for (int x = 0; x < a.w; x++, p += 3) {
            p[0] = lr[p[0]];
            p[1] = lg[p[1]];
            p[2] = lb[p[2]];
        }
    }
}

void img_multiply(Img a, Img b) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        const uint8_t *q = PX(b, 0, y);
        if (b.c == a.c) {
            for (int i = 0; i < a.w * a.c; i++) p[i] = (uint8_t)((int)p[i] * q[i] / 255);
        } else {                                  /* grey mask on a colour picture */
            for (int x = 0; x < a.w; x++, p += 3) {
                int m = q[x];
                p[0] = (uint8_t)((int)p[0] * m / 255);
                p[1] = (uint8_t)((int)p[1] * m / 255);
                p[2] = (uint8_t)((int)p[2] * m / 255);
            }
        }
    }
}

void img_add_modulo(Img a, Img b) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        const uint8_t *q = PX(b, 0, y);
        for (int i = 0; i < a.w * a.c; i++) p[i] = (uint8_t)(p[i] + q[i]);
    }
}

void img_add(Img a, Img b, float scale, int offset) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        const uint8_t *q = PX(b, 0, y);
        for (int i = 0; i < a.w * a.c; i++) {
            int t = (int)(((int)p[i] + (int)q[i]) / scale + offset);
            p[i] = t <= 0 ? 0 : t >= 255 ? 255 : t;
        }
    }
}

Img img_offset(Img a, int dx, int dy) {
    Img o = img_new(a.w, a.h, a.c);
    dx %= a.w;
    if (dx < 0) dx += a.w;
    dy %= a.h;
    if (dy < 0) dy += a.h;
    for (int y = 0; y < a.h; y++) {
        int ys = (y - dy + a.h) % a.h;
        for (int x = 0; x < a.w; x++) {
            int xs = (x - dx + a.w) % a.w;
            memcpy(PX(o, x, y), PX(a, xs, ys), a.c);
        }
    }
    return o;
}

void img_composite(Img a, Img b, Img mask) {
    for (int y = 0; y < a.h; y++) {
        uint8_t *p = PX(a, 0, y);
        const uint8_t *q = PX(b, 0, y), *m = PX(mask, 0, y);
        for (int x = 0; x < a.w; x++) {
            int mm = m[x];
            for (int ch = 0; ch < a.c; ch++) {
                unsigned t = q[x * a.c + ch] * (255 - mm) + p[x * a.c + ch] * mm + 128;
                p[x * a.c + ch] = (uint8_t)(((t >> 8) + t) >> 8);
            }
        }
    }
}

static inline uint8_t clip8f(float v) {
    if (v <= 0.0f) return 0;
    if (v >= 255.0f) return 255;
    return (uint8_t)v;
}

Img img_filter3x3(Img a, const float k0[9], float scale, float offset) {
    float k[9];
    for (int i = 0; i < 9; i++) k[i] = (float)((double)k0[i] / scale);
    Img o = img_new(a.w, a.h, 1);
    if (a.w < 3 || a.h < 3) {
        img_free(&o);
        return img_copy(a);
    }
    offset += 0.5f;
    memcpy(PX(o, 0, 0), PX(a, 0, 0), a.w);
    for (int y = 1; y < a.h - 1; y++) {
        const uint8_t *in_1 = PX(a, 0, y - 1), *in0 = PX(a, 0, y), *in1 = PX(a, 0, y + 1);
        uint8_t *out = PX(o, 0, y);
        out[0] = in0[0];
        int x;
        for (x = 1; x < a.w - 1; x++) {
            float ss = offset;
            ss += (float)in1[x - 1] * k[0] + (float)in1[x] * k[1] + (float)in1[x + 1] * k[2];
            ss += (float)in0[x - 1] * k[3] + (float)in0[x] * k[4] + (float)in0[x + 1] * k[5];
            ss += (float)in_1[x - 1] * k[6] + (float)in_1[x] * k[7] + (float)in_1[x + 1] * k[8];
            out[x] = clip8f(ss);
        }
        out[x] = in0[x];
    }
    memcpy(PX(o, 0, a.h - 1), PX(a, 0, a.h - 1), a.w);
    return o;
}

/* ------------------------------------------------------------- conversions */
Img img_to_l(Img a) {
    if (a.c == 1) return img_copy(a);
    Img o = img_new(a.w, a.h, 1);
    for (int y = 0; y < a.h; y++) {
        const uint8_t *p = PX(a, 0, y);
        uint8_t *q = PX(o, 0, y);
        for (int x = 0; x < a.w; x++, p += 3)
            q[x] = (uint8_t)((p[0] * 19595 + p[1] * 38470 + p[2] * 7471 + 0x8000) >> 16);
    }
    return o;
}

Img img_to_rgb(Img a) {
    if (a.c == 3) return img_copy(a);
    Img o = img_new(a.w, a.h, 3);
    for (int y = 0; y < a.h; y++) {
        const uint8_t *p = PX(a, 0, y);
        uint8_t *q = PX(o, 0, y);
        for (int x = 0; x < a.w; x++, q += 3) q[0] = q[1] = q[2] = p[x];
    }
    return o;
}


void rgb_px_to_ycc(int r, int g, int b, uint8_t *out) { rgb2ycc(r, g, b, out); }
void ycc_px_to_rgb(int y, int cb, int cr, uint8_t *out) { ycc2rgb(y, cb, cr, out); }

void img_rgb_to_ycc(Img rgb, Img *y, Img *cb, Img *cr) {
    *y = img_alloc(rgb.w, rgb.h, 1);
    *cb = img_alloc(rgb.w, rgb.h, 1);
    *cr = img_alloc(rgb.w, rgb.h, 1);
    img_rgb_to_ycc_into(rgb, *y, *cb, *cr);
}

/* into three planes of rgb's size (e.g. views into the video encoder's buffer) */
void img_rgb_to_ycc_into(Img rgb, Img y, Img cb, Img cr) {
    for (int j = 0; j < rgb.h; j++) {
        const uint8_t *p = PX(rgb, 0, j);
        uint8_t *a = PX(y, 0, j), *b = PX(cb, 0, j), *c = PX(cr, 0, j);
        for (int x = 0; x < rgb.w; x++, p += 3) {
            uint8_t t[3];
            rgb2ycc(p[0], p[1], p[2], t);
            a[x] = t[0]; b[x] = t[1]; c[x] = t[2];
        }
    }
}

/* Y at full size, Cb and Cr at half size - the full-size ones reduce(2)'d,
 * (a + b + c + d + 2) >> 2 like Pillow - in one pass (video frames) */
void img_rgb_to_yuv420(Img rgb, Img *y, Img *cb, Img *cr) {
    int w = rgb.w, h = rgb.h;
    if ((w | h) & 1) {                               /* (odd sizes: Pillow's edge rules) */
        Img u, v;
        img_rgb_to_ycc(rgb, y, &u, &v);
        *cb = img_reduce(u, 2);
        *cr = img_reduce(v, 2);
        img_free(&u);
        img_free(&v);
        return;
    }
    *y = img_alloc(w, h, 1);
    *cb = img_alloc(w / 2, h / 2, 1);
    *cr = img_alloc(w / 2, h / 2, 1);
    img_rgb_to_yuv420_into(rgb, *y, *cb, *cr);
}

/* the same into given planes (even sizes only) */
void img_rgb_to_yuv420_into(Img rgb, Img Y, Img Cb, Img Cr) {
    int w = rgb.w, h = rgb.h;
    for (int j = 0; j < h; j += 2) {
        const uint8_t *p0 = PX(rgb, 0, j), *p1 = PX(rgb, 0, j + 1);
        uint8_t *y0 = PX(Y, 0, j), *y1 = PX(Y, 0, j + 1), *u = PX(Cb, 0, j / 2), *v = PX(Cr, 0, j / 2);
        for (int x = 0; x < w; x += 2, p0 += 6, p1 += 6) {
            uint8_t a[3], b[3], c[3], d[3];
            rgb2ycc(p0[0], p0[1], p0[2], a);
            rgb2ycc(p0[3], p0[4], p0[5], b);
            rgb2ycc(p1[0], p1[1], p1[2], c);
            rgb2ycc(p1[3], p1[4], p1[5], d);
            y0[x] = a[0]; y0[x + 1] = b[0];
            y1[x] = c[0]; y1[x + 1] = d[0];
            u[x / 2] = (uint8_t)((a[1] + b[1] + c[1] + d[1] + 2) >> 2);
            v[x / 2] = (uint8_t)((a[2] + b[2] + c[2] + d[2] + 2) >> 2);
        }
    }
}

Img img_ycc_to_rgb(Img y, Img cb, Img cr) {
    Img o = img_alloc(y.w, y.h, 3);
    for (int j = 0; j < y.h; j++) {
        const uint8_t *a = PX(y, 0, j), *b = PX(cb, 0, j), *c = PX(cr, 0, j);
        uint8_t *q = PX(o, 0, j);
        for (int x = 0; x < y.w; x++, q += 3) ycc2rgb(a[x], b[x], c[x], q);
    }
    return o;
}

/* resize_nearest's source position for each of dw places (-1: none) */
void img_nearest_map(int sw, int dw, int *xs) {
    double a0 = (double)sw / dw, xo = a0 * 0.5;
    for (int x = 0; x < dw; x++, xo += a0) {
        int xin = xo < 0.0 ? -1 : (int)xo;
        xs[x] = (xin >= 0 && xin < sw) ? xin : -1;
    }
}

/* Where each output pixel of "turn (90 degrees clockwise) if asked, then
 * resize_nearest to tw x th" comes from, as two offsets into the plane: one
 * per output column, one per output row (-1: no source - black). */
static void view_offsets(int pw, int ph, int stride, int turn, int tw, int th, int *ox, int *oy) {
    int Tw = turn ? ph : pw, Th = turn ? pw : ph;
    int *mx = malloc(sizeof(int) * tw), *my = malloc(sizeof(int) * th);
    img_nearest_map(Tw, tw, mx);
    img_nearest_map(Th, th, my);
    for (int x = 0; x < tw; x++)                   /* turned (X, Y) = plane (Y, ph-1-X) */
        ox[x] = mx[x] < 0 ? -1 : turn ? (ph - 1 - mx[x]) * stride : mx[x];
    for (int y = 0; y < th; y++)
        oy[y] = my[y] < 0 ? -1 : turn ? my[y] : my[y] * stride;
    free(mx);
    free(my);
}

/* The live view's picture straight from the camera's planes (U, V at half
 * size): turned if asked, nearest-resized to tw x th, R,G,B - in one pass,
 * exactly what turning the three planes, enlarging U and V to Y's size
 * (nearest), converting and resizing to tw x th (nearest) would give. */
Img img_view_rgb(Img Y, Img U, Img V, int turn, int tw, int th) {
    Img o = img_new(tw, th, 3);
    int Tw = turn ? Y.h : Y.w, Th = turn ? Y.w : Y.h;
    int TUw = turn ? U.h : U.w, TUh = turn ? U.w : U.h;
    int *mx = malloc(sizeof(int) * tw), *my = malloc(sizeof(int) * th);
    int *ux = malloc(sizeof(int) * Tw), *uy = malloc(sizeof(int) * Th);
    int *oyx = malloc(sizeof(int) * tw), *cj = malloc(sizeof(int) * tw);
    /* a chroma row's colour terms, once per chroma sample (shared by the 2 x 2
     * pixels it covers), + one more for "no chroma" (cb = cr = 0) */
    int *dr = malloc(sizeof(int) * (TUw + 1) * 3), *dg = dr + TUw + 1, *db = dg + TUw + 1;
    img_nearest_map(Tw, tw, mx);
    img_nearest_map(Th, th, my);
    img_nearest_map(TUw, Tw, ux);                  /* U, V enlarged to Y's size */
    img_nearest_map(TUh, Th, uy);
    for (int x = 0; x < tw; x++) {
        int X = mx[x];
        oyx[x] = X < 0 ? -1 : turn ? (Y.h - 1 - X) * Y.stride : X;
        int Xu = X < 0 ? -1 : ux[X];
        cj[x] = Xu < 0 ? TUw : Xu;
    }
    dr[TUw] = R_Cr[0] >> SCALE;
    dg[TUw] = (G_Cb[0] + G_Cr[0]) >> SCALE;
    db[TUw] = B_Cb[0] >> SCALE;
    int last = -2;
    for (int y = 0; y < th; y++) {
        int Yt = my[y];
        if (Yt < 0) continue;                      /* (black) */
        const uint8_t *py = Y.p + (turn ? (size_t)Yt : (size_t)Yt * Y.stride);
        int Yu = uy[Yt];
        if (Yu != last) {                          /* this chroma row's terms */
            last = Yu;
            for (int j = 0; j < TUw; j++) {
                int cb = 0, cr = 0;
                if (Yu >= 0) {                     /* turned: TU(j, Yu) = U(Yu, uh-1-j) */
                    size_t off = turn ? (size_t)(U.h - 1 - j) * U.stride + Yu : (size_t)Yu * U.stride + j;
                    cb = U.p[off];
                    cr = V.p[turn ? (size_t)(V.h - 1 - j) * V.stride + Yu : (size_t)Yu * V.stride + j];
                }
                dr[j] = R_Cr[cr] >> SCALE;
                dg[j] = (G_Cb[cb] + G_Cr[cr]) >> SCALE;
                db[j] = B_Cb[cb] >> SCALE;
            }
        }
        uint8_t *q = PX(o, 0, y);
        for (int x = 0; x < tw; x++, q += 3) {
            if (oyx[x] < 0) continue;
            int yv = py[oyx[x]], j = cj[x];
            int r = yv + dr[j], g = yv + dg[j], b = yv + db[j];
            q[0] = r <= 0 ? 0 : r >= 255 ? 255 : r;
            q[1] = g <= 0 ? 0 : g >= 255 ? 255 : g;
            q[2] = b <= 0 ? 0 : b >= 255 ? 255 : b;
        }
    }
    free(mx); free(my); free(ux); free(uy); free(oyx); free(cj); free(dr);
    return o;
}

/* one plane, turned if asked and nearest-resized to tw x th, in one pass */
Img img_view_plane(Img a, int turn, int tw, int th) {
    Img o = img_new(tw, th, 1);
    int *ox = malloc(sizeof(int) * tw), *oy = malloc(sizeof(int) * th);
    view_offsets(a.w, a.h, a.stride, turn, tw, th, ox, oy);
    for (int y = 0; y < th; y++) {
        if (oy[y] < 0) continue;
        const uint8_t *s = a.p + oy[y];
        uint8_t *q = PX(o, 0, y);
        if (!turn && ox[0] == 0 && tw == a.w) {    /* (as it is) */
            memcpy(q, s, tw);
            continue;
        }
        for (int x = 0; x < tw; x++)
            if (ox[x] >= 0) q[x] = s[ox[x]];
    }
    free(ox);
    free(oy);
    return o;
}

/* ----------------------------------------------------------------- drawing */
void img_rect(Img a, int x0, int y0, int x1, int y1, int r, int g, int b) {
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= a.w) x1 = a.w - 1;
    if (y1 >= a.h) y1 = a.h - 1;
    for (int y = y0; y <= y1; y++) {
        uint8_t *p = PX(a, x0, y);
        for (int x = x0; x <= x1; x++, p += a.c) {
            p[0] = r;
            if (a.c == 3) { p[1] = g; p[2] = b; }
        }
    }
}

void img_vline(Img a, int x, int y0, int y1, int width, int r, int g, int b) {
    if (width < 1) width = 1;
    int xa = x - (width - 1) / 2;
    img_rect(a, xa, y0, xa + width - 1, y1, r, g, b);
}

/* ---------------------------------------------------------------- turning */
/* Turning in tiles of 16 x 16 pixels (the Pi Zero's small cache: the
 * picture is read along rows and written along columns), pixel by pixel
 * without a function call each. */
#define ROT_TILE 16
static void rot_copy(Img a, Img o, int cw) {
    int c = a.c;
    for (int ty = 0; ty < a.h; ty += ROT_TILE) {
        int ye = ty + ROT_TILE < a.h ? ty + ROT_TILE : a.h;
        for (int tx = 0; tx < a.w; tx += ROT_TILE) {
            int xe = tx + ROT_TILE < a.w ? tx + ROT_TILE : a.w;
            for (int y = ty; y < ye; y++) {
                const uint8_t *s = PX(a, tx, y);
                /* cw: (x, y) -> (h-1-y, x); ccw: (x, y) -> (y, w-1-x) */
                if (c == 1) {
                    if (cw) {
                        uint8_t *d = o.p + (size_t)tx * o.stride + (a.h - 1 - y);
                        for (int x = tx; x < xe; x++, d += o.stride) *d = *s++;
                    } else {
                        uint8_t *d = o.p + (size_t)(a.w - 1 - tx) * o.stride + y;
                        for (int x = tx; x < xe; x++, d -= o.stride) *d = *s++;
                    }
                } else {
                    if (cw) {
                        uint8_t *d = o.p + (size_t)tx * o.stride + (size_t)(a.h - 1 - y) * c;
                        for (int x = tx; x < xe; x++, d += o.stride, s += c)
                            for (int k = 0; k < c; k++) d[k] = s[k];
                    } else {
                        uint8_t *d = o.p + (size_t)(a.w - 1 - tx) * o.stride + (size_t)y * c;
                        for (int x = tx; x < xe; x++, d -= o.stride, s += c)
                            for (int k = 0; k < c; k++) d[k] = s[k];
                    }
                }
            }
        }
    }
}

Img img_rot90cw(Img a) {
    Img o = img_alloc(a.h, a.w, a.c);
    rot_copy(a, o, 1);
    return o;
}

Img img_rot90ccw(Img a) {
    Img o = img_alloc(a.h, a.w, a.c);
    rot_copy(a, o, 0);
    return o;
}

Img img_flip(Img a, int hflip, int vflip) {
    Img o = img_alloc(a.w, a.h, a.c);
    int c = a.c;
    for (int y = 0; y < a.h; y++) {
        int ys = vflip ? a.h - 1 - y : y;
        const uint8_t *s = PX(a, 0, ys);
        uint8_t *d = PX(o, 0, y);
        if (!hflip) {
            memcpy(d, s, (size_t)a.w * c);
            continue;
        }
        const uint8_t *e = s + (size_t)(a.w - 1) * c;
        for (int x = 0; x < a.w; x++, d += c, e -= c)
            for (int k = 0; k < c; k++) d[k] = e[k];
    }
    return o;
}

Img img_rot180(Img a) { return img_flip(a, 1, 1); }
