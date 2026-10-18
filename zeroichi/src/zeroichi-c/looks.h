/* Zero Ichi - the looks (filters). A port of the Python Zero Ichi's filters.py:
 * the same tables, palettes and steps, so photos, GIFs and videos look the
 * same as before. */
#ifndef ZEROICHI_LOOKS_H
#define ZEROICHI_LOOKS_H
#include <stdint.h>
#include "img.h"

typedef struct {
    uint8_t r[256], g[256], b[256];   /* tone -> colour */
    uint8_t y[256], u[256], v[256];   /* tone -> JPEG Y, Cb, Cr */
    uint8_t gif_index[256];           /* tone -> GIF colour number */
    int gray;                         /* r == g == b */
    int ncolors;
} Palette;

enum {
    LOOK_NORMAL, LOOK_8MM, LOOK_HOLGA, LOOK_HOLGA_COLOR, LOOK_GB, LOOK_GB_BW,
    LOOK_PX_ARCADE, LOOK_PX_NEON, LOOK_PX_PAPER,
    LOOK_MONO, LOOK_HICON, LOOK_CROSS, LOOK_THERMAL, LOOK_PRISM, LOOK_PRISM_3, LOOK_PRISM_RAINBOW,
    LOOK_ZEROICHI, NLOOKS
};
extern const char *const LOOK_NAMES[NLOOKS];
int look_by_name(const char *name);          /* -1 if unknown (old names mapped) */
int look_is_tone(int id);                    /* Holga, GB, GB B&W, Mono, Hi-Con, Thermal */
int look_is_pixel(int id);                   /* the 16-Bit looks (palette numbers on a grid) */
int look_is_yuv(int id);                     /* Normal, Cross */
int look_half_ok(int id);                    /* developing may decode at half size */
int look_square(int id);                     /* Holga B&W, Holga Color: always square */

/* A black & white style result: grey tones (or palette numbers) + palette */
typedef struct {
    Img t;
    const Palette *pal;                      /* NULL = plain grey */
    int w, h;                                /* full size */
    int indexed;                             /* t holds palette numbers */
    int sharp;                               /* enlarge with sharp pixel edges */
} Tone;

void looks_init(void);
extern int looks_test_mode;                  /* no grain, fixed dice (for tests) */
extern double looks_test_dice;               /* (the fixed dice: 0.5; < 0: a sequence) */
void looks_test_seed(unsigned seed);

/* black & white looks: grey (or R,G,B) picture in; out_w/out_h = 0: same size */
Tone tone_make(int id, Img src, int out_w, int out_h);
/* 16-Bit: from Y/U/V planes (U, V half or full size) or from an R,G,B picture */
Tone pixel_tone_planes(int id, Img Y, Img U, Img V, int out_w, int out_h);
Tone pixel_tone_rgb(int id, Img rgb, int out_w, int out_h);
void tone_free(Tone *t);
Img tone_rgb(Tone *t);                       /* R,G,B at its size */
void tone_yuv(Tone *t, int w, int h, int smooth, Img *Y, Img *U, Img *V);
Img tone_gif(Tone *t, const Palette **pal);  /* GIF colour numbers at its size */
const Palette *tone_palette(Tone *t);

/* any look on an R,G,B picture (Python FILTERS[i](img)); out size for 8mm */
Img look_rgb(int id, Img rgb, int out_w, int out_h);
/* a finished photo from the camera's Y, U, V planes (U, V half size):
 * returns 1 with *Y,*U,*V planes, or 0 with *rgb */
int look_photo(int id, Img Y, Img U, Img V, Img *oy, Img *ou, Img *ov, Img *rgb);
Img eightmm(Img rgb, int out_w, int out_h);

/* Zero Ichi (v52: colour film - filters.zeroichi_*): all on Y, U, V */
Img zi_luma(Img y);                          /* the film's brightness (curve, grain, vignette) */
void zi_colour(Img T, Img U, Img V, Img *ou, Img *ov);   /* T: graded brightness at U's size */
void zi_yuv(Img Y, Img U, Img V, Img *oy, Img *ou, Img *ov);   /* U, V half size */
void hc_yuv(Img Y, Img U, Img V, Img *oy, Img *ou, Img *ov);   /* (v55) Holga Color on Y, U, V */
extern const uint8_t T_ZI_TONE[256], T_ZI_YC[256], T_ZI_EXP[256];
extern const int16_t ZI_SY[256];
extern const int8_t ZI_TU[256], ZI_TV[256], ZI_CU[16384], ZI_CV[16384];

/* live view helpers */
const uint16_t *yuv565_table(int id);        /* Normal / Cross */
const uint16_t *palette_565(const Palette *p);

/* tables (tables.c, generated) */
extern const uint8_t T_MONO[256], T_CROSS_R[256], T_CROSS_G[256], T_CROSS_B[256],
    T_HOLGA_C[256], T_HICON[256], T_HC_Y[256], T_HC_CB[256], T_HC_CR[256], T_GB_CONTRAST[256], T_EM[17 * 768];
extern const Palette PAL_GRAY, PAL_HOLGA, PAL_THERMAL, PAL_GB_GREEN, PAL_GB_GRAY,
    PAL_PIXEL_0, PAL_PIXEL_1, PAL_PIXEL_2;
extern const uint8_t LUT_PIXEL_0[32768], LUT_PIXEL_1[32768], LUT_PIXEL_2[32768];
extern const int AMP_PIXEL_0, AMP_PIXEL_1, AMP_PIXEL_2, NCOL_PIXEL_0, NCOL_PIXEL_1, NCOL_PIXEL_2;
extern const uint8_t JPEG_STD_DHT[];
extern const int JPEG_STD_DHT_LEN;

/* small helpers */
int pyround(double v);                       /* Python's round() */
uint32_t rnd_u32(void);
double rnd01(void);                          /* random 0..1 (fixed 0.5 in test mode) */
int rnd_pick(int n);
void pixel_grid(int w, int h, int *gw, int *gh);
#endif
