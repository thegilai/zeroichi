#include "mov.h"
#include "meta.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "jpeg.h"
#include "util.h"

#define TIMESCALE 1000

static void wr(MovWriter *m, const void *d, size_t n) {
    fwrite(d, 1, n, m->f);
    m->pos += n;
    m->kb = (uint32_t)(m->pos >> 10);
}

int mov_open(MovWriter *m, const char *path, int w, int h, int rotation, const char *look) {
    memset(m, 0, sizeof *m);
    if (look) snprintf(m->look, sizeof m->look, "%s", look);
    m->f = fopen(path, "wb");
    if (!m->f) return -1;
    setvbuf(m->f, NULL, _IOFBF, 1 << 16);
    m->w = w;
    m->h = h;
    m->rotation = rotation;
    static const uint8_t ftyp[] = {0, 0, 0, 20, 'f', 't', 'y', 'p', 'q', 't', ' ', ' ', 0, 0, 2, 0, 'q', 't', ' ', ' '};
    wr(m, ftyp, sizeof ftyp);
    m->mdat_pos = m->pos;
    static const uint8_t mdat[] = {0, 0, 0, 0, 'm', 'd', 'a', 't'};
    wr(m, mdat, sizeof mdat);
    return 0;
}

int mov_add(MovWriter *m, const uint8_t *jpeg, size_t len, double t) {
    Buf fixed = {0};
    if (jpeg_needs_huffman(jpeg, len)) {
        jpeg_add_huffman(jpeg, len, &fixed);
        jpeg = fixed.p;
        len = fixed.n;
    }
    if (m->n == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 256;
        m->sizes = realloc(m->sizes, sizeof(uint32_t) * m->cap);
        m->offsets = realloc(m->offsets, sizeof(uint32_t) * m->cap);
        m->times = realloc(m->times, sizeof(double) * m->cap);
    }
    uint64_t pos = m->pos;
    size_t w = fwrite(jpeg, 1, len, m->f);
    m->pos += w;
    m->kb = (uint32_t)(m->pos >> 10);
    buf_free(&fixed);
    if (w != len) return -1;
    m->offsets[m->n] = (uint32_t)pos;
    m->sizes[m->n] = (uint32_t)len;
    m->times[m->n] = t;
    m->n++;
    return 0;
}

double mov_duration(const MovWriter *m) { return m->n > 1 ? m->times[m->n - 1] - m->times[0] : 0.0; }

static size_t atom_begin(Buf *b, const char *kind) {
    size_t at = b->n;
    buf_be32(b, 0);
    buf_add(b, kind, 4);
    return at;
}

static void atom_end(Buf *b, size_t at) {
    uint32_t n = (uint32_t)(b->n - at);
    b->p[at] = (uint8_t)(n >> 24);
    b->p[at + 1] = (uint8_t)(n >> 16);
    b->p[at + 2] = (uint8_t)(n >> 8);
    b->p[at + 3] = (uint8_t)n;
}

static void full(Buf *b, int version, int flags) { buf_be32(b, (uint32_t)((version << 24) | flags)); }

static void matrix(Buf *b, int rotation, int w, int h) {
    (void)w;
    if (rotation == 90) {
        uint32_t m[9] = {0, 0x10000, 0, 0xFFFF0000u, 0, 0, (uint32_t)h << 16, 0, 0x40000000};
        for (int i = 0; i < 9; i++) buf_be32(b, m[i]);
    } else {
        uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
        for (int i = 0; i < 9; i++) buf_be32(b, m[i]);
    }
}

static void zeros(Buf *b, int n) {
    for (int i = 0; i < n; i++) buf_u8(b, 0);
}

int mov_close(MovWriter *m) {
    uint64_t end = m->pos;
    fflush(m->f);
    fseek(m->f, (long)m->mdat_pos, SEEK_SET);
    uint32_t sz = (uint32_t)(end - m->mdat_pos);
    uint8_t s4[4] = {(uint8_t)(sz >> 24), (uint8_t)(sz >> 16), (uint8_t)(sz >> 8), (uint8_t)sz};
    fwrite(s4, 1, 4, m->f);
    fseek(m->f, (long)end, SEEK_SET);
    if (m->n) {
        int n = m->n;
        uint32_t *durs = malloc(sizeof(uint32_t) * n);
        uint64_t total = 0;
        for (int i = 0; i + 1 < n; i++) {
            long d = lround((m->times[i + 1] - m->times[i]) * TIMESCALE);
            durs[i] = d < 1 ? 1 : (uint32_t)d;
            total += durs[i];
        }
        durs[n - 1] = n > 1 ? (uint32_t)lround((double)total / (n - 1)) : 100;
        total += durs[n - 1];
        int nruns = 0;
        uint32_t *rc = malloc(sizeof(uint32_t) * n), *rd = malloc(sizeof(uint32_t) * n);
        for (int i = 0; i < n; i++) {
            if (nruns && rd[nruns - 1] == durs[i]) rc[nruns - 1]++;
            else { rc[nruns] = 1; rd[nruns++] = durs[i]; }
        }
        Buf b = {0};
        size_t moov = atom_begin(&b, "moov");
        size_t a = atom_begin(&b, "mvhd");
        full(&b, 0, 0);
        buf_be32(&b, 0); buf_be32(&b, 0); buf_be32(&b, TIMESCALE); buf_be32(&b, (uint32_t)total);
        buf_be32(&b, 0x10000); buf_be16(&b, 0x100);
        zeros(&b, 10);
        matrix(&b, 0, 0, 0);
        zeros(&b, 24);
        buf_be32(&b, 2);
        atom_end(&b, a);
        size_t trak = atom_begin(&b, "trak");
        a = atom_begin(&b, "tkhd");
        full(&b, 0, 3);
        buf_be32(&b, 0); buf_be32(&b, 0); buf_be32(&b, 1); buf_be32(&b, 0); buf_be32(&b, (uint32_t)total);
        zeros(&b, 8);
        buf_be16(&b, 0); buf_be16(&b, 0); buf_be16(&b, 0); buf_be16(&b, 0);
        matrix(&b, m->rotation, m->w, m->h);
        buf_be32(&b, (uint32_t)m->w << 16);
        buf_be32(&b, (uint32_t)m->h << 16);
        atom_end(&b, a);
        size_t mdia = atom_begin(&b, "mdia");
        a = atom_begin(&b, "mdhd");
        full(&b, 0, 0);
        buf_be32(&b, 0); buf_be32(&b, 0); buf_be32(&b, TIMESCALE); buf_be32(&b, (uint32_t)total);
        buf_be16(&b, 0x55C4); buf_be16(&b, 0);
        atom_end(&b, a);
        a = atom_begin(&b, "hdlr");
        full(&b, 0, 0);
        buf_add(&b, "mhlrvide", 8);
        zeros(&b, 12);
        buf_add(&b, "\x0cVideoHandler", 13);
        atom_end(&b, a);
        size_t minf = atom_begin(&b, "minf");
        a = atom_begin(&b, "vmhd");
        full(&b, 0, 1);
        zeros(&b, 8);
        atom_end(&b, a);
        size_t dinf = atom_begin(&b, "dinf");
        size_t dref = atom_begin(&b, "dref");
        full(&b, 0, 0);
        buf_be32(&b, 1);
        size_t alis = atom_begin(&b, "alis");
        full(&b, 0, 1);
        atom_end(&b, alis);
        atom_end(&b, dref);
        atom_end(&b, dinf);
        size_t stbl = atom_begin(&b, "stbl");
        size_t stsd = atom_begin(&b, "stsd");
        full(&b, 0, 0);
        buf_be32(&b, 1);
        size_t e = atom_begin(&b, "jpeg");
        zeros(&b, 6);
        buf_be16(&b, 1);
        buf_be16(&b, 0); buf_be16(&b, 0); buf_add(&b, "appl", 4); buf_be32(&b, 0); buf_be32(&b, 512);
        buf_be16(&b, m->w); buf_be16(&b, m->h); buf_be32(&b, 72 << 16); buf_be32(&b, 72 << 16);
        buf_be32(&b, 0); buf_be16(&b, 1);
        const char *name = "Photo - JPEG";
        buf_u8(&b, (int)strlen(name));
        buf_add(&b, name, strlen(name));
        zeros(&b, 31 - (int)strlen(name));
        buf_be16(&b, 24); buf_be16(&b, 0xFFFF);
        atom_end(&b, e);
        atom_end(&b, stsd);
        a = atom_begin(&b, "stts");
        full(&b, 0, 0);
        buf_be32(&b, (uint32_t)nruns);
        for (int i = 0; i < nruns; i++) { buf_be32(&b, rc[i]); buf_be32(&b, rd[i]); }
        atom_end(&b, a);
        a = atom_begin(&b, "stsc");
        full(&b, 0, 0);
        buf_be32(&b, 1); buf_be32(&b, 1); buf_be32(&b, 1); buf_be32(&b, 1);
        atom_end(&b, a);
        a = atom_begin(&b, "stsz");
        full(&b, 0, 0);
        buf_be32(&b, 0);
        buf_be32(&b, (uint32_t)n);
        for (int i = 0; i < n; i++) buf_be32(&b, m->sizes[i]);
        atom_end(&b, a);
        a = atom_begin(&b, "stco");
        full(&b, 0, 0);
        buf_be32(&b, (uint32_t)n);
        for (int i = 0; i < n; i++) buf_be32(&b, m->offsets[i]);
        atom_end(&b, a);
        atom_end(&b, stbl);
        atom_end(&b, minf);
        atom_end(&b, mdia);
        atom_end(&b, trak);
        {                                            /* who made it, the look - no date (meta.h) */
            char desc[96];
            snprintf(desc, sizeof desc, META_LOOK "%s", m->look);
            const char *const keys[4] = {"\xa9mak", "\xa9mod", "\xa9swr", "\xa9" "cmt"};   /* (comment; split: "c" is a hex digit) */
            const char *const texts[4] = {META_MAKE, META_MODEL, META_SOFTWARE, desc};
            size_t udta = atom_begin(&b, "udta");
            for (int i = 0; i < (m->look[0] ? 4 : 3); i++) {
                a = atom_begin(&b, keys[i]);
                buf_be16(&b, (int)strlen(texts[i]));
                buf_be16(&b, 0x55C4);                /* (language: undetermined) */
                buf_add(&b, texts[i], strlen(texts[i]));
                atom_end(&b, a);
            }
            atom_end(&b, udta);
        }
        atom_end(&b, moov);
        fwrite(b.p, 1, b.n, m->f);
        buf_free(&b);
        free(durs);
        free(rc);
        free(rd);
    }
    fflush(m->f);
    fsync(fileno(m->f));
    int r = fclose(m->f);
    m->f = NULL;
    free(m->sizes);
    free(m->offsets);
    free(m->times);
    m->sizes = m->offsets = NULL;
    m->times = NULL;
    return r;
}

/* ----------------------------------------------------------------- reader */
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

/* find an atom path inside [start, end) of d: returns the body range */
static int find(const uint8_t *d, size_t start, size_t end, const char *const *path, int depth,
                size_t *bs, size_t *be) {
    size_t pos = start;
    while (pos + 8 <= end) {
        uint32_t size = be32(d + pos);
        if (size < 8 || pos + size > end) return -1;
        if (!memcmp(d + pos + 4, path[0], 4)) {
            if (depth == 1) {
                *bs = pos + 8;
                *be = pos + size;
                return 0;
            }
            return find(d, pos + 8, pos + size, path + 1, depth - 1, bs, be);
        }
        pos += size;
    }
    return -1;
}

int movr_open(MovReader *r, const char *path) {
    memset(r, 0, sizeof *r);
    r->f = fopen(path, "rb");
    if (!r->f) return -1;
    fseek(r->f, 0, SEEK_END);
    uint64_t end = (uint64_t)ftell(r->f), pos = 0;
    uint8_t *moov = NULL;
    size_t mlen = 0;
    while (pos + 8 <= end) {
        uint8_t h[16];
        fseek(r->f, (long)pos, SEEK_SET);
        if (fread(h, 1, 8, r->f) != 8) break;
        uint64_t size = be32(h);
        int hdr = 8;
        if (size == 1) {
            if (fread(h + 8, 1, 8, r->f) != 8) break;
            size = (uint64_t)be32(h + 8) << 32 | be32(h + 12);
            hdr = 16;
        } else if (size == 0) {
            size = end - pos;
        }
        if (size < (uint64_t)hdr) break;
        if (!memcmp(h + 4, "moov", 4)) {
            mlen = (size_t)(size - hdr);
            moov = malloc(mlen);
            if (fread(moov, 1, mlen, r->f) != mlen) {
                free(moov);
                moov = NULL;
            }
            break;
        }
        pos += size;
    }
    if (!moov) {
        fclose(r->f);
        r->f = NULL;
        return -1;
    }
    size_t bs, be;
    const char *stbl[] = {"trak", "mdia", "minf", "stbl"};
    size_t ss, se;
    if (find(moov, 0, mlen, stbl, 4, &ss, &se)) goto bad;
    const char *p_stsz[] = {"stsz"}, *p_stco[] = {"stco"}, *p_stts[] = {"stts"};
    if (find(moov, ss, se, p_stsz, 1, &bs, &be)) goto bad;
    uint32_t fixed = be32(moov + bs + 4), n = be32(moov + bs + 8);
    if (bs + 12 + (size_t)(fixed ? 0 : n) * 4 > be) goto bad;
    r->n = (int)n;
    r->sizes = malloc(sizeof(uint32_t) * (n + 1));
    for (uint32_t i = 0; i < n; i++) r->sizes[i] = fixed ? fixed : be32(moov + bs + 12 + i * 4);
    if (find(moov, ss, se, p_stco, 1, &bs, &be)) goto bad;
    uint32_t m = be32(moov + bs + 4);
    if (m < n || bs + 8 + (size_t)n * 4 > be) goto bad;
    r->offsets = malloc(sizeof(uint32_t) * (n + 1));
    for (uint32_t i = 0; i < n; i++) r->offsets[i] = be32(moov + bs + 8 + i * 4);
    if (find(moov, ss, se, p_stts, 1, &bs, &be)) goto bad;
    uint32_t runs = be32(moov + bs + 4);
    r->durations = calloc(n + 1, sizeof(double));
    uint32_t k = 0;
    for (uint32_t i = 0; i < runs && bs + 16 + i * 8 <= be; i++) {
        uint32_t c = be32(moov + bs + 8 + i * 8), d = be32(moov + bs + 12 + i * 8);
        for (uint32_t j = 0; j < c && k < n; j++) r->durations[k++] = d / (double)TIMESCALE;
    }
    /* size and rotation from the track header */
    const char *p_tkhd[] = {"trak", "tkhd"};
    r->w = 320;
    r->h = 240;
    if (!find(moov, 0, mlen, p_tkhd, 2, &bs, &be) && be - bs >= 84) {
        r->w = (int)(be32(moov + be - 8) >> 16);
        r->h = (int)(be32(moov + be - 4) >> 16);
        int32_t a = (int32_t)be32(moov + be - 44), b = (int32_t)be32(moov + be - 40);
        if (a == 0 && b > 0) r->rotation = 90;
        else if (a == 0 && b < 0) r->rotation = 270;
        else if (a < 0) r->rotation = 180;
    }
    free(moov);
    return 0;
bad:
    free(moov);
    movr_close(r);
    return -1;
}

uint8_t *movr_frame(MovReader *r, int i, size_t *len) {
    if (i < 0 || i >= r->n) return NULL;
    uint8_t *p = malloc(r->sizes[i] + 1);
    fseek(r->f, (long)r->offsets[i], SEEK_SET);
    if (fread(p, 1, r->sizes[i], r->f) != r->sizes[i]) {
        free(p);
        return NULL;
    }
    *len = r->sizes[i];
    return p;
}

double movr_duration(const MovReader *r) {
    double s = 0;
    for (int i = 0; i < r->n; i++) s += r->durations[i];
    return s;
}

void movr_close(MovReader *r) {
    if (r->f) fclose(r->f);
    free(r->sizes);
    free(r->offsets);
    free(r->durations);
    memset(r, 0, sizeof *r);
}
