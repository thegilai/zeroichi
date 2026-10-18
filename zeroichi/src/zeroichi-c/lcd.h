/* Zero Ichi - the SPI screen (ST7789 / ILI9341), the same set-up as lcd.py and
 * zeroichi-lcd. Frames are sent by a background thread while the next one is
 * being made. */
#ifndef ZEROICHI_LCD_H
#define ZEROICHI_LCD_H
#include <stdint.h>

typedef struct {
    char profile[32], controller[32];
    int w, h, madctl, invert, dc, rst, bl, cs, speed, bits, flip;
} LcdConf;

int lcd_profile(const char *name, LcdConf *c);   /* 0 = known screen */
int lcd_open(const LcdConf *c, int reset);       /* 0 = ok */
void lcd_show(const uint16_t *px);               /* native RGB565, w*h; sent in the background */
void lcd_show_now(const uint16_t *px);           /* (waits until it's on the screen) */
void lcd_wait(void);
void lcd_sleep(void);
void lcd_wake(void);
void lcd_backlight(int on);
void lcd_release(void);                          /* free the SPI port, keep the picture */
void lcd_close(void);                            /* black, backlight off, panel asleep */
int lcd_write_helper_conf(const char *path);     /* settings for zeroichi-lcd */
int lcd_helper_conf_same(const char *path);      /* 1: that file says so already */
const LcdConf *lcd_conf(void);
extern volatile int lcd_frames_sent;
/* tests: every frame sent is also written to this file (raw RGB565) */
void lcd_sim_dump(const char *path);
#endif
