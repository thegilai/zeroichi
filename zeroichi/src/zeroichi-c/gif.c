#include "gif.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "meta.h"
#include "util.h"

/* ------------------------------------------------------------- writing */
typedef struct {
    Buf *out;
    uint8_t block[256];
    int nblock;
    uint32_t acc;
    int nacc;
} Bits;

static void flush_block(Bits *b) {
    if (!b->nblock) return;
    buf_u8(b->out, b->nblock);
    buf_add(b->out, b->block, (size_t)b->nblock);
    b->nblock = 0;
}

static void put_code(Bits *b, int code, int size) {
    b->acc |= (uint32_t)code << b->nacc;
    b->nacc += size;
    while (b->nacc >= 8) {
        b->block[b->nblock++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->nacc -= 8;
        if (b->nblock == 255) flush_block(b);
    }
}

/* LZW with a hash table (string = prefix code + next colour) */
static void lzw(Buf *out, const uint8_t *px, size_t n, int min_size) {
    int clear = 1 << min_size, eoi = clear + 1;
    int size = min_size + 1, next = eoi + 1;
    enum { HS = 8192 };
    int32_t *keys = malloc(sizeof(int32_t) * HS);
    int16_t *vals = malloc(sizeof(int16_t) * HS);
    memset(keys, 0xFF, sizeof(int32_t) * HS);
    Bits b = {out, {0}, 0, 0, 0};
    buf_u8(out, min_size);
    put_code(&b, clear, size);
    if (n) {
        int cur = px[0];
        for (size_t i = 1; i < n; i++) {
            int c = px[i];
            int32_t key = (cur << 8) | c;
            unsigned h = ((unsigned)key * 2654435761u) >> 19;
            while (keys[h] >= 0 && keys[h] != key) h = (h + 1) & (HS - 1);
            if (keys[h] == key) {
                cur = vals[h];
                continue;
            }
            put_code(&b, cur, size);
            if (next < 4096) {
                keys[h] = key;
                vals[h] = (int16_t)next;
                if (next == (1 << size) && size < 12) size++;
                next++;
            } else {                               /* table full: start again */
                put_code(&b, clear, size);
                memset(keys, 0xFF, sizeof(int32_t) * HS);
                size = min_size + 1;
                next = eoi + 1;
            }
            cur = c;
        }
        put_code(&b, cur, size);
    }
    put_code(&b, eoi, size);
    if (b.nacc) {
        b.block[b.nblock++] = (uint8_t)b.acc;
        if (b.nblock == 255) flush_block(&b);
    }
    flush_block(&b);
    buf_u8(out, 0);
    free(keys);
    free(vals);
}

int gif_write(const char *path, Img *frames, int n, const uint8_t (*pal)[3], int ncolors, int delay_ms,
              const char *look) {
    if (n < 1) return -1;
    int w = frames[0].w, h = frames[0].h;
    int bits = 1;
    while ((1 << bits) < ncolors) bits++;
    Buf o = {0};
    buf_add(&o, "GIF89a", 6);
    buf_le16(&o, w);
    buf_le16(&o, h);
    buf_u8(&o, 0x80 | 0x70 | (bits - 1));          /* global table */
    buf_u8(&o, 0);
    buf_u8(&o, 0);
    for (int i = 0; i < (1 << bits); i++) {
        uint8_t c[3] = {0, 0, 0};
        if (i < ncolors) memcpy(c, pal[i], 3);
        buf_add(&o, c, 3);
    }
    static const uint8_t loop[] = {0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0',
                                   0x03, 0x01, 0x00, 0x00, 0x00};
    buf_add(&o, loop, sizeof loop);
    {                                                /* a comment: who made it and the look (meta.h) */
        char made[200];
        int len = snprintf(made, sizeof made, "Made with " META_SOFTWARE "%s%s",
                           look && look[0] ? "\n" META_LOOK : "", look && look[0] ? look : "");
        if (len > 255) len = 255;                    /* (one sub-block) */
        if (len > (int)sizeof made - 1) len = (int)sizeof made - 1;
        buf_u8(&o, 0x21);
        buf_u8(&o, 0xFE);
        buf_u8(&o, len);
        buf_add(&o, made, (size_t)len);
        buf_u8(&o, 0);
    }
    int cs = (delay_ms + 5) / 10;
    uint8_t *px = malloc((size_t)w * h);
    for (int f = 0; f < n; f++) {
        uint8_t gce[] = {0x21, 0xF9, 0x04, 0x00, (uint8_t)cs, (uint8_t)(cs >> 8), 0x00, 0x00};
        buf_add(&o, gce, sizeof gce);
        buf_u8(&o, 0x2C);
        buf_le16(&o, 0);
        buf_le16(&o, 0);
        buf_le16(&o, w);
        buf_le16(&o, h);
        buf_u8(&o, 0);
        for (int y = 0; y < h; y++) memcpy(px + (size_t)y * w, PX(frames[f], 0, y), (size_t)w);
        lzw(&o, px, (size_t)w * h, bits < 2 ? 2 : bits);
    }
    free(px);
    buf_u8(&o, 0x3B);
    int r = write_file(path, o.p, o.n, 1);
    buf_free(&o);
    return r;
}

/* --------------------------------------------------- shared palette (median cut) */
/* (v53: the same palette as before, about 15 times quicker: each box keeps its
 *  colours' ranges, and a box is split by a counting sort - in order, like the
 *  sort used before, but a few passes instead of a comparison sort; the
 *  nearest colour is only worked out for the colours that occur) */
typedef struct { int lo, hi, range[3]; } Box;

static void box_ranges(const uint8_t *s, Box *b) {
    int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
    for (int k = b->lo; k < b->hi; k++) {
        const uint8_t *p = s + (size_t)k * 3;
        for (int ch = 0; ch < 3; ch++) {
            if (p[ch] < mn[ch]) mn[ch] = p[ch];
            if (p[ch] > mx[ch]) mx[ch] = p[ch];
        }
    }
    for (int ch = 0; ch < 3; ch++) b->range[ch] = b->hi > b->lo ? mx[ch] - mn[ch] : 0;
}

/* the box's samples in order of channel ch (keeping their order otherwise) */
static void box_sort(uint8_t *s, uint8_t *tmp, const Box *b, int ch) {
    size_t count[257] = {0};
    for (int k = b->lo; k < b->hi; k++) count[s[(size_t)k * 3 + ch] + 1]++;
    for (int v = 0; v < 256; v++) count[v + 1] += count[v];
    for (int k = b->lo; k < b->hi; k++) {
        const uint8_t *p = s + (size_t)k * 3;
        memcpy(tmp + count[p[ch]]++ * 3, p, 3);
    }
    memcpy(s + (size_t)b->lo * 3, tmp, (size_t)(b->hi - b->lo) * 3);
}

int gif_quantize(Img *rgb, int n, int maxcol, uint8_t (*pal)[3], Img *idx) {
    /* samples: every 4th pixel of every 4th row of every frame */
    size_t cap = 0;
    for (int f = 0; f < n; f++) cap += (size_t)((rgb[f].w + 3) / 4) * ((rgb[f].h + 3) / 4);
    uint8_t *s = malloc(cap * 3 + 3), *tmp = malloc(cap * 3 + 3);
    size_t ns = 0;
    for (int f = 0; f < n; f++)
        for (int y = 0; y < rgb[f].h; y += 4) {
            const uint8_t *p = PX(rgb[f], 0, y);
            for (int x = 0; x < rgb[f].w; x += 4, ns++) memcpy(s + ns * 3, p + x * 3, 3);
        }
    Box boxes[256];
    int nb = 1;
    if (maxcol > 256) maxcol = 256;
    boxes[0] = (Box){0, (int)ns, {0, 0, 0}};
    box_ranges(s, &boxes[0]);
    while (nb < maxcol) {
        int best = -1, bch = 0, brange = 0;
        for (int i = 0; i < nb; i++) {
            if (boxes[i].hi - boxes[i].lo < 2) continue;
            for (int ch = 0; ch < 3; ch++)
                if (boxes[i].range[ch] > brange) {
                    brange = boxes[i].range[ch];
                    best = i;
                    bch = ch;
                }
        }
        if (best < 0 || brange == 0) break;
        box_sort(s, tmp, &boxes[best], bch);
        int mid = (boxes[best].lo + boxes[best].hi) / 2;
        boxes[nb] = (Box){mid, boxes[best].hi, {0, 0, 0}};
        boxes[best].hi = mid;
        box_ranges(s, &boxes[best]);
        box_ranges(s, &boxes[nb]);
        nb++;
    }
    for (int i = 0; i < nb; i++) {
        long sum[3] = {0, 0, 0};
        int cnt = boxes[i].hi - boxes[i].lo;
        for (int k = boxes[i].lo; k < boxes[i].hi; k++)
            for (int ch = 0; ch < 3; ch++) sum[ch] += s[(size_t)k * 3 + ch];
        for (int ch = 0; ch < 3; ch++) pal[i][ch] = cnt ? (uint8_t)((sum[ch] + cnt / 2) / cnt) : 0;
    }
    if (!ns) {
        nb = 1;
        memset(pal[0], 0, 3);
    }
    free(s);
    free(tmp);
    /* the nearest colour for each 5-5-5 colour - worked out when it first occurs */
    uint8_t *inv = malloc(32768), known[4096];
    memset(known, 0, sizeof known);
    for (int f = 0; f < n; f++) {
        idx[f] = img_new(rgb[f].w, rgb[f].h, 1);
        for (int y = 0; y < rgb[f].h; y++) {
            const uint8_t *p = PX(rgb[f], 0, y);
            uint8_t *o = PX(idx[f], 0, y);
            for (int x = 0; x < rgb[f].w; x++, p += 3) {
                int c = (p[0] >> 3) << 10 | (p[1] >> 3) << 5 | (p[2] >> 3);
                if (!(known[c >> 3] & (1 << (c & 7)))) {
                    int r = ((c >> 10) & 31) * 8 + 4, g = ((c >> 5) & 31) * 8 + 4, b = (c & 31) * 8 + 4;
                    int bi = 0, bd = 1 << 30;
                    for (int i = 0; i < nb; i++) {
                        int dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
                        int d = dr * dr * 3 + dg * dg * 4 + db * db * 2;
                        if (d < bd) {
                            bd = d;
                            bi = i;
                        }
                    }
                    inv[c] = (uint8_t)bi;
                    known[c >> 3] |= (uint8_t)(1 << (c & 7));
                }
                o[x] = inv[c];
            }
        }
    }
    free(inv);
    return nb;
}

/* ------------------------------------------------------------- reading */
struct GifReader {
    uint8_t *d;
    size_t n, pos;
    int w, h, bg;
    uint8_t gpal[256][3];
    int gcount;
    Img canvas, prev;
    int disposal, prev_disposal, transp, delay;
    int px0, py0, pw, ph;
};

GifReader *gifr_open(const char *path) {
    size_t n;
    uint8_t *d = (uint8_t *)read_file(path, &n);
    if (!d || n < 13 || memcmp(d, "GIF8", 4)) {
        free(d);
        return NULL;
    }
    GifReader *g = calloc(1, sizeof *g);
    g->d = d;
    g->n = n;
    g->w = d[6] | d[7] << 8;
    g->h = d[8] | d[9] << 8;
    if (g->w < 1 || g->h < 1 || g->w > 4096 || g->h > 4096) {
        gifr_close(g);
        return NULL;
    }
    int flags = d[10];
    g->bg = d[11];
    g->pos = 13;
    if (flags & 0x80) {
        g->gcount = 2 << (flags & 7);
        if (g->pos + g->gcount * 3 > n) {
            gifr_close(g);
            return NULL;
        }
        memcpy(g->gpal, d + g->pos, (size_t)g->gcount * 3);
        g->pos += (size_t)g->gcount * 3;
    }
    g->canvas = img_new(g->w, g->h, 3);
    g->transp = -1;
    return g;
}

int gifr_size(GifReader *g, int *w, int *h) {
    *w = g->w;
    *h = g->h;
    return 0;
}

static int lzw_decode(const uint8_t *d, size_t n, size_t *pos, int min_size, uint8_t *out, size_t want) {
    /* gather the data sub-blocks */
    size_t p = *pos, total = 0;
    Buf data = {0};
    while (p < n) {
        int len = d[p++];
        if (!len) break;
        if (p + len > n) len = (int)(n - p);
        buf_add(&data, d + p, (size_t)len);
        p += len;
        total += len;
    }
    *pos = p;
    if (min_size < 2 || min_size > 11) {
        buf_free(&data);
        return -1;
    }
    int clear = 1 << min_size, eoi = clear + 1, size = min_size + 1, next = eoi + 1;
    static __thread uint16_t prefix[4096];
    static __thread uint8_t suffix[4096], stack[4097];
    for (int i = 0; i < clear; i++) {
        prefix[i] = 0xFFFF;
        suffix[i] = (uint8_t)i;
    }
    size_t o = 0, bitpos = 0, nbits = total * 8;
    int old = -1;
    uint8_t first = 0;
    while (o < want && bitpos + size <= nbits) {
        int code = 0;
        for (int i = 0; i < size; i++, bitpos++)
            code |= ((data.p[bitpos >> 3] >> (bitpos & 7)) & 1) << i;
        if (code == clear) {
            size = min_size + 1;
            next = eoi + 1;
            old = -1;
            continue;
        }
        if (code == eoi) break;
        int c = code, sp = 0;
        if (old < 0) {
            if (code >= clear) break;
            out[o++] = (uint8_t)code;
            first = (uint8_t)code;
            old = code;
            continue;
        }
        if (code >= next) {                         /* (the KwKwK case) */
            stack[sp++] = first;
            c = old;
        }
        while (c >= clear && sp < 4096) {
            stack[sp++] = suffix[c];
            c = prefix[c];
        }
        stack[sp++] = (uint8_t)c;
        first = (uint8_t)c;
        while (sp && o < want) out[o++] = stack[--sp];
        if (next < 4096) {
            prefix[next] = (uint16_t)old;
            suffix[next] = first;
            next++;
            if (next == (1 << size) && size < 12) size++;
        }
        old = code;
    }
    buf_free(&data);
    return (int)o;
}

int gifr_next(GifReader *g, Img *rgb, double *seconds) {
    const uint8_t *d = g->d;
    /* what the last frame left behind */
    if (g->disposal == 2) {
        uint8_t c[3] = {0, 0, 0};
        for (int y = g->py0; y < g->py0 + g->ph && y < g->h; y++)
            for (int x = g->px0; x < g->px0 + g->pw && x < g->w; x++) memcpy(PX(g->canvas, x, y), c, 3);
    } else if (g->disposal == 3 && g->prev.p) {
        Img t = g->canvas;
        g->canvas = g->prev;
        g->prev = t;
    }
    g->disposal = 0;
    g->transp = -1;
    g->delay = 10;
    while (g->pos < g->n) {
        int b = d[g->pos++];
        if (b == 0x3B) return 0;
        if (b == 0x21 && g->pos < g->n) {
            int label = d[g->pos++];
            if (label == 0xF9 && g->pos + 5 < g->n && d[g->pos] == 4) {
                int fl = d[g->pos + 1];
                g->delay = d[g->pos + 2] | d[g->pos + 3] << 8;
                g->disposal = (fl >> 2) & 7;
                g->transp = (fl & 1) ? d[g->pos + 4] : -1;
            }
            while (g->pos < g->n && d[g->pos]) g->pos += d[g->pos] + 1;
            g->pos++;
            continue;
        }
        if (b != 0x2C || g->pos + 9 > g->n) return 0;
        int x0 = d[g->pos] | d[g->pos + 1] << 8, y0 = d[g->pos + 2] | d[g->pos + 3] << 8;
        int w = d[g->pos + 4] | d[g->pos + 5] << 8, h = d[g->pos + 6] | d[g->pos + 7] << 8;
        int fl = d[g->pos + 8];
        g->pos += 9;
        uint8_t (*pal)[3] = g->gpal;
        uint8_t lpal[256][3];
        if (fl & 0x80) {
            int lc = 2 << (fl & 7);
            if (g->pos + lc * 3 > g->n) return 0;
            memcpy(lpal, d + g->pos, (size_t)lc * 3);
            g->pos += (size_t)lc * 3;
            pal = lpal;
        }
        if (g->pos >= g->n) return 0;
        int min_size = d[g->pos++];
        uint8_t *px = calloc((size_t)w * h + 1, 1);
        lzw_decode(d, g->n, &g->pos, min_size, px, (size_t)w * h);
        if (g->disposal == 3) {
            if (g->prev.p) img_free(&g->prev);
            g->prev = img_copy(g->canvas);
        }
        int interlaced = fl & 0x40;
        static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
        int row = 0;
        for (int pass = 0; pass < (interlaced ? 4 : 1); pass++)
            for (int yy = interlaced ? start[pass] : 0; yy < h; yy += interlaced ? step[pass] : 1, row++) {
                int y = y0 + yy;
                if (y >= g->h) continue;
                for (int xx = 0; xx < w; xx++) {
                    int x = x0 + xx;
                    if (x >= g->w) continue;
                    int c = px[(size_t)row * w + xx];
                    if (c == g->transp) continue;
                    memcpy(PX(g->canvas, x, y), pal[c], 3);
                }
            }
        free(px);
        g->px0 = x0;
        g->py0 = y0;
        g->pw = w;
        g->ph = h;
        *rgb = g->canvas;
        double s = g->delay / 100.0;
        *seconds = s < 0.04 ? 0.04 : s;
        return 1;
    }
    return 0;
}

void gifr_close(GifReader *g) {
    if (!g) return;
    free(g->d);
    img_free(&g->canvas);
    if (g->prev.p) img_free(&g->prev);
    free(g);
}
