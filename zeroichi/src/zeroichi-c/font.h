/* Zero Ichi - text: DejaVu Sans Bold letters pre-rendered by Pillow (fontdata.c) */
#ifndef ZEROICHI_FONT_H
#define ZEROICHI_FONT_H
#include <stddef.h>
#include <stdint.h>

typedef struct { int16_t x, y, w, h; int32_t off; } GlyphMask;   /* from the pen (baseline) */
typedef struct { int32_t cp, adv64; GlyphMask fill, stroke; } Glyph;
typedef struct {
    int size, stroke, ascent, descent, n;
    const Glyph *g;
    const unsigned char *bits;                    /* NULL: FONT_BITS (DejaVu) */
    int bold;
} Font;

extern const Font FONTS[];
extern const int NFONTS;
extern const unsigned char FONT_BITS[];

const Font *font(int size);                       /* nearest size available */
/* (v52) Zero Ichi's own letters for its menus: Poppins (fontbrand.c) */
extern const Font BRAND_FONTS[];
extern const int NBRAND_FONTS;
const Font *brand_font(int size, int bold);       /* nearest size of that weight */
const Glyph *font_glyph(const Font *f, uint32_t cp);
double text_width(const Font *f, const char *s);  /* Pillow's getlength() */
uint32_t utf8_next(const char **s);
#endif
