/* Zero Ichi - the screen saver's two pictures (brandart.c, made by
 * gen/gen_brand.py from the Bauhaus design) */
#ifndef ZEROICHI_BRANDART_H
#define ZEROICHI_BRANDART_H
#include <stdint.h>

typedef struct BrandArt {
    int dx, dy, w, h;          /* placed at (middle + dx, ... + dy) */
    const uint8_t *rgba;       /* w * h * 4: R, G, B, alpha */
} BrandArt;

extern const BrandArt BR_MARK;   /* the sleeping "0 1" (dy from the screen's middle) */
extern const BrandArt BR_TEXT;   /* "zero ichi - press any key" (dy from the bottom edge) */
extern const BrandArt BR_KANA;   /* (v53) "ゼロイチ" in red, for MENU > About (dx, dy: 0) */
#endif
