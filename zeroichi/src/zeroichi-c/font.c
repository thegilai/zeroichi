#include "font.h"
#include <stdlib.h>

const Font *font(int size) {
    const Font *best = &FONTS[0];
    for (int i = 0; i < NFONTS; i++)
        if (abs(FONTS[i].size - size) < abs(best->size - size)) best = &FONTS[i];
    return best;
}

const Font *brand_font(int size, int bold) {
    const Font *best = NULL;
    for (int i = 0; i < NBRAND_FONTS; i++) {
        const Font *f = &BRAND_FONTS[i];
        if (f->bold != bold) continue;
        if (!best || abs(f->size - size) < abs(best->size - size)) best = f;
    }
    return best ? best : font(size);
}

const Glyph *font_glyph(const Font *f, uint32_t cp) {
    int lo = 0, hi = f->n - 1;                    /* (sorted, except the extra symbols) */
    if (cp >= 32 && cp < 127 && f->n > 94 && f->g[cp - 32].cp == (int32_t)cp) return &f->g[cp - 32];
    for (int i = lo; i <= hi; i++)
        if (f->g[i].cp == (int32_t)cp) return &f->g[i];
    if (cp != '?') return font_glyph(f, '?');
    return NULL;
}

uint32_t utf8_next(const char **ps) {
    const unsigned char *s = (const unsigned char *)*ps;
    uint32_t c = s[0];
    int n = 1;
    if (c >= 0xF0 && s[1] && s[2] && s[3]) {
        c = ((c & 7) << 18) | ((s[1] & 63) << 12) | ((s[2] & 63) << 6) | (s[3] & 63);
        n = 4;
    } else if (c >= 0xE0 && s[1] && s[2]) {
        c = ((c & 15) << 12) | ((s[1] & 63) << 6) | (s[2] & 63);
        n = 3;
    } else if (c >= 0xC0 && s[1]) {
        c = ((c & 31) << 6) | (s[1] & 63);
        n = 2;
    }
    *ps += n;
    return c;
}

double text_width(const Font *f, const char *s) {
    int64_t w = 0;
    while (*s) {
        const Glyph *g = font_glyph(f, utf8_next(&s));
        if (g) w += g->adv64;
    }
    return w / 64.0;
}
