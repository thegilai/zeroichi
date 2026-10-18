#include "lcd.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "gpio.h"
#include "util.h"

#define SPI_IOC_WR_MODE          0x40016b01u
#define SPI_IOC_WR_BITS_PER_WORD 0x40016b03u
#define SPI_IOC_WR_MAX_SPEED_HZ  0x40046b04u

static LcdConf C;
static int spi = -1, sim, opened;
static size_t chunk = 4096;
static pthread_mutex_t bus = PTHREAD_MUTEX_INITIALIZER;        /* one user of the SPI at a time */
static pthread_mutex_t q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t q_cond = PTHREAD_COND_INITIALIZER;
static uint8_t *sendbuf[2];
static size_t sendlen;
static int next_buf, pending = -1, busy;
static pthread_t th;
static int colmod = 0x55, asleep;
volatile int lcd_frames_sent;
static char sim_dump[512];

void lcd_sim_dump(const char *path) { snprintf(sim_dump, sizeof sim_dump, "%s", path); }
const LcdConf *lcd_conf(void) { return &C; }

int lcd_profile(const char *name, LcdConf *c) {
    static const LcdConf P[] = {
        {"1.3inch-hat", "st7789", 240, 240, 0x70, 1, 25, 27, 24, 0, 40000000, 16, 0},
        {"2.8inch-hat", "st7789-2.8inch", 320, 240, 0x68, 1, 25, 27, 24, 0, 40000000, 16, 0},
        {"st7789-320x240", "st7789", 320, 240, 0x60, 1, 25, 27, 18, 0, 40000000, 16, 0},
        {"ili9341-320x240", "ili9341", 320, 240, 0x28, 0, 25, 27, 18, 0, 40000000, 16, 0},
    };
    for (unsigned i = 0; i < sizeof P / sizeof P[0]; i++)
        if (!strcmp(name, P[i].profile)) {
            *c = P[i];
            return 0;
        }
    *c = P[0];
    return -1;
}

static int spi_write(const uint8_t *p, size_t n) {
    if (sim) return 0;
    while (n) {
        size_t k = n < chunk ? n : chunk;
        ssize_t w = write(spi, p, k);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int cmd(uint8_t c, const uint8_t *d, size_t n) {
    gpio_write(C.dc, 0);
    if (spi_write(&c, 1)) return -1;
    if (n) {
        gpio_write(C.dc, 1);
        if (spi_write(d, n)) return -1;
    }
    return 0;
}

#define CMD(c, ...) do { static const uint8_t d_[] = {__VA_ARGS__}; cmd(c, d_, sizeof d_); } while (0)
#define CMD0(c) cmd(c, NULL, 0)

static void init_st7789(void) {
    CMD(0x3A, 0x05);
    CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);
    CMD(0xB7, 0x35);
    CMD(0xBB, 0x19);
    CMD(0xC0, 0x2C);
    CMD(0xC2, 0x01);
    CMD(0xC3, 0x12);
    CMD(0xC4, 0x20);
    CMD(0xC6, 0x0F);
    CMD(0xD0, 0xA4, 0xA1);
    CMD(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23);
    CMD(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23);
}

/* "st7789-..." (not plain "st7789"): the 2.8inch hat's panel, which needs its
 * own set-up - sent twice, as its maker's driver does */
static int hat28(const char *controller) { return !strncmp(controller, "st7789-", 7); }

static void init_hat28(void) {
    CMD0(0x11);
    msleep(120);
    CMD0(0x13);
    CMD(0xB6, 0x0A, 0x82);
    CMD(0x3A, 0x55);
    msleep(10);
    CMD(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33);
    CMD(0xB7, 0x35);
    CMD(0xBB, 0x28);
    CMD(0xC0, 0x0C);
    CMD(0xC2, 0x01, 0xFF);
    CMD(0xC3, 0x10);
    CMD(0xC4, 0x20);
    CMD(0xC6, 0x0F);
    CMD(0xD0, 0xA4, 0xA1);
    CMD(0xE0, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17);
    CMD(0xE1, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E);
    CMD0(0x21);
    CMD0(0x29);
    msleep(120);
}

static void init_ili9341(void) {
    CMD(0xEF, 0x03, 0x80, 0x02);
    CMD(0xCF, 0x00, 0xC1, 0x30);
    CMD(0xED, 0x64, 0x03, 0x12, 0x81);
    CMD(0xE8, 0x85, 0x00, 0x78);
    CMD(0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02);
    CMD(0xF7, 0x20);
    CMD(0xEA, 0x00, 0x00);
    CMD(0xC0, 0x23);
    CMD(0xC1, 0x10);
    CMD(0xC5, 0x3E, 0x28);
    CMD(0xC7, 0x86);
    CMD(0x3A, 0x55);
    CMD(0xB1, 0x00, 0x18);
    CMD(0xB6, 0x08, 0x82, 0x27);
    CMD(0xF2, 0x00);
    CMD(0x26, 0x01);
    CMD(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00);
    CMD(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F);
}

static void init_panel(int reset) {
    if (!reset) {                         /* already set up (by zeroichi-lcd, or before): */
        uint8_t md = (uint8_t)C.madctl;   /* just make sure of the essentials, no waits */
        cmd(0x36, &md, 1);
        CMD0(C.invert ? 0x21 : 0x20);
        CMD(0x3A, 0x55);
        CMD0(0x29);
        colmod = 0x55;
        return;
    }
    {
        gpio_write(C.rst, 1);
        msleep(10);
        gpio_write(C.rst, 0);
        msleep(10);
        gpio_write(C.rst, 1);
        msleep(120);
    }
    if (!strcmp(C.controller, "ili9341")) {
        init_ili9341();
    } else if (hat28(C.controller)) {
        init_hat28();
        init_hat28();
    } else {
        init_st7789();
    }
    uint8_t md = (uint8_t)C.madctl;
    cmd(0x36, &md, 1);
    CMD0(C.invert ? 0x21 : 0x20);
    if (!hat28(C.controller)) {
        CMD0(0x11);
        msleep(120);
        CMD0(0x29);
    }
    colmod = 0x55;
}

static void set_colmod(int v) {
    if (colmod != v) {
        uint8_t b = (uint8_t)v;
        cmd(0x3A, &b, 1);
        colmod = v;
    }
}

static void write_frame(const uint8_t *buf, size_t n) {
    pthread_mutex_lock(&bus);
    set_colmod(C.bits == 18 ? 0x66 : 0x55);
    uint8_t xs[4] = {0, 0, (uint8_t)((C.w - 1) >> 8), (uint8_t)((C.w - 1) & 0xFF)};
    uint8_t ys[4] = {0, 0, (uint8_t)((C.h - 1) >> 8), (uint8_t)((C.h - 1) & 0xFF)};
    cmd(0x2A, xs, 4);
    cmd(0x2B, ys, 4);
    cmd(0x2C, buf, n);
    pthread_mutex_unlock(&bus);
    if (sim_dump[0]) write_file_atomic(sim_dump, buf, n);
    lcd_frames_sent++;
}

static void *sender(void *arg) {
    (void)arg;
    pthread_mutex_lock(&q_lock);
    for (;;) {
        while (pending < 0) pthread_cond_wait(&q_cond, &q_lock);
        int i = pending;
        pending = -1;
        busy = 1;
        pthread_mutex_unlock(&q_lock);
        write_frame(sendbuf[i], sendlen);
        pthread_mutex_lock(&q_lock);
        busy = 0;
        pthread_cond_broadcast(&q_cond);
    }
    return NULL;
}

int lcd_open(const LcdConf *c, int reset) {
    C = *c;
    sim = getenv("ZEROICHI_SIM") != NULL;
    if (sim && getenv("ZEROICHI_SIM_DUMP")) lcd_sim_dump(getenv("ZEROICHI_SIM_DUMP"));
    if (gpio_open() && !sim) return -1;
    if (!sim) {
        char dev[32];
        snprintf(dev, sizeof dev, "/dev/spidev0.%d", C.cs ? 1 : 0);
        for (int i = 0; i < 150 && access(dev, F_OK); i++) msleep(10);
        if (spi >= 0) close(spi);                  /* (opened again: another screen) */
        spi = open(dev, O_RDWR | O_CLOEXEC);
        if (spi < 0) return -1;
        uint8_t mode = 0, bits = 8;
        uint32_t speed = (uint32_t)C.speed;
        ioctl(spi, SPI_IOC_WR_MODE, &mode);
        ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits);
        ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed);
        FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
        long v = 0;
        if (f) {
            if (fscanf(f, "%ld", &v) != 1) v = 0;
            fclose(f);
        }
        if (v >= 4096 && v <= (1 << 20)) chunk = (size_t)v;
    }
    gpio_output(C.dc);
    gpio_output(C.rst);
    gpio_output(C.bl);
    if (!opened) {
        size_t n = (size_t)480 * 480 * 3;
        sendbuf[0] = malloc(n);
        sendbuf[1] = malloc(n);
        pthread_create(&th, NULL, sender, NULL);
        opened = 1;
    }
    pthread_mutex_lock(&bus);
    init_panel(reset);
    pthread_mutex_unlock(&bus);
    asleep = 0;
    return 0;
}

static void convert(const uint16_t *px, uint8_t *out) {
    int n = C.w * C.h;
    if (C.bits == 18) {
        for (int i = 0; i < n; i++) {
            uint16_t v = px[C.flip ? n - 1 - i : i];
            out[3 * i] = (uint8_t)((v >> 8) & 0xF8);
            out[3 * i + 1] = (uint8_t)((v >> 3) & 0xFC);
            out[3 * i + 2] = (uint8_t)(v << 3);
        }
        sendlen = (size_t)n * 3;
        return;
    }
    uint16_t *o = (uint16_t *)out;
    if (C.flip) {
        for (int i = 0; i < n; i++) o[i] = __builtin_bswap16(px[n - 1 - i]);
    } else {
        for (int i = 0; i < n; i++) o[i] = __builtin_bswap16(px[i]);
    }
    sendlen = (size_t)n * 2;
}

void lcd_show(const uint16_t *px) {
    pthread_mutex_lock(&q_lock);
    while (pending >= 0) pthread_cond_wait(&q_cond, &q_lock);   /* (never two waiting) */
    int i = next_buf;
    pthread_mutex_unlock(&q_lock);
    convert(px, sendbuf[i]);                        /* the buffer that isn't being sent */
    pthread_mutex_lock(&q_lock);
    while (busy || pending >= 0) pthread_cond_wait(&q_cond, &q_lock);
    pending = i;
    next_buf = i ^ 1;
    pthread_cond_broadcast(&q_cond);
    pthread_mutex_unlock(&q_lock);
}

void lcd_wait(void) {
    pthread_mutex_lock(&q_lock);
    while (busy || pending >= 0) pthread_cond_wait(&q_cond, &q_lock);
    pthread_mutex_unlock(&q_lock);
}

void lcd_show_now(const uint16_t *px) {
    lcd_show(px);
    lcd_wait();
}

void lcd_backlight(int on) {
    if (sim) fprintf(stderr, "LCD: backlight %s\n", on ? "on" : "off");
    gpio_write(C.bl, on);
}

void lcd_sleep(void) {
    lcd_wait();
    pthread_mutex_lock(&bus);
    gpio_write(C.bl, 0);
    CMD0(0x28);
    CMD0(0x10);
    pthread_mutex_unlock(&bus);
    asleep = 1;
    if (sim) fprintf(stderr, "LCD: sleep\n");
}

void lcd_wake(void) {
    if (!asleep) return;
    pthread_mutex_lock(&bus);
    CMD0(0x11);
    msleep(120);
    CMD0(0x29);
    pthread_mutex_unlock(&bus);
    asleep = 0;
    if (sim) fprintf(stderr, "LCD: wake\n");
}

void lcd_release(void) {
    lcd_wait();
    if (sim) fprintf(stderr, "LCD: released\n");
    if (spi >= 0) close(spi);
    spi = -1;
}

void lcd_close(void) {
    lcd_wait();
    size_t n = (size_t)C.w * C.h;
    uint16_t *black = calloc(n, 2);
    lcd_show_now(black);
    free(black);
    pthread_mutex_lock(&bus);
    gpio_write(C.bl, 0);
    CMD0(0x28);
    CMD0(0x10);
    pthread_mutex_unlock(&bus);
    lcd_release();
}

static int helper_conf(char *s, size_t cap) {
    return snprintf(s, cap,
                    "controller %s\nwidth %d\nheight %d\nmadctl %d\ninvert %d\ndc %d\nrst %d\n"
                    "bl %d\ncs %d\nspeed %d\nflip %d\n",
                    C.controller, C.w, C.h, C.madctl, C.invert ? 1 : 0, C.dc, C.rst, C.bl, C.cs,
                    C.speed, C.flip ? 1 : 0);
}

int lcd_helper_conf_same(const char *path) {
    char s[512];
    int n = helper_conf(s, sizeof s);
    size_t len;
    char *old = read_file(path, &len);
    int same = old && len == (size_t)n && !memcmp(old, s, n);
    free(old);
    return same;
}

int lcd_write_helper_conf(const char *path) {
    char s[512];
    int n = helper_conf(s, sizeof s);
    if (lcd_helper_conf_same(path)) return 0;
    return write_file_atomic(path, s, n);
}
