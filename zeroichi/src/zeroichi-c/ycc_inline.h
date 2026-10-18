/* Zero Ichi - Pillow's YCbCr <-> R,G,B for one pixel (whole numbers, its own
 * tables - pillow_ycc.h), inline: for the passes that do everything per pixel */
#ifndef YCC_INLINE_H
#define YCC_INLINE_H
#include <stdint.h>
#include "pillow_ycc.h"

static inline void rgb2ycc(int r, int g, int b, uint8_t *out) {
    out[0] = (uint8_t)((Y_R[r] + Y_G[g] + Y_B[b]) >> SCALE);
    out[1] = (uint8_t)(((Cb_R[r] + Cb_G[g] + Cb_B[b]) >> SCALE) + 128);
    out[2] = (uint8_t)(((Cr_R[r] + Cr_G[g] + Cr_B[b]) >> SCALE) + 128);
}

static inline void ycc2rgb(int y, int cb, int cr, uint8_t *out) {
    int r = y + ((R_Cr[cr]) >> SCALE);
    int g = y + ((G_Cb[cb] + G_Cr[cr]) >> SCALE);
    int b = y + ((B_Cb[cb]) >> SCALE);
    out[0] = r <= 0 ? 0 : r >= 255 ? 255 : r;
    out[1] = g <= 0 ? 0 : g >= 255 ? 255 : g;
    out[2] = b <= 0 ? 0 : b >= 255 ? 255 : b;
}
#endif
