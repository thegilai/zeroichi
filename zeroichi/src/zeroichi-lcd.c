/*
 * zeroichi-lcd: show one picture on Zero Ichi's SPI screen, as early as possible.
 *
 * Zero Ichi's fast start runs this about 4 seconds after power-on - long before
 * Python and the camera are loaded - so the start-up picture appears at once
 * (the snake-os way: a tiny C program, no libraries). It is also used to show
 * a message while the fast start does something that takes a while.
 *
 *   zeroichi-lcd [--direct] [--update] [--progress N] CONFIG IMAGE [IMAGE...]
 *   zeroichi-lcd --held PIN        (exit 0: that button is held down)
 *
 * (v52) It also does a few small start-up jobs for the fast start, each in
 * this one tiny program instead of several big ones (mount, mkdir, date, stat,
 * dd, od, findfs... each takes a moment to load on a Pi Zero, and the start
 * is waiting for the processor). Each prints what it did; anything else (an
 * older zeroichi-lcd) and the fast start does it the old way:
 *   --early PIN       mount /proc, /sys (and /dev); KEY1 held?  "ok held|free|unknown"
 *   --mount-run       /run, /run/lock, /run/zeroichi, /dev/pts, /dev/shm, /tmp, the
 *                     host name                                    "ok"
 *   --clock FILE...   the clock on from the newest of these (no clock battery) "ok"
 *   --fat-dirty DEV   that FAT drive wasn't closed properly?       "dirty|clean"
 *   --partuuid SPEC   PARTUUID=xxxxxxxx-NN -> its device            "/dev/..."
 *   --wait FILE SECS  (v53) wait until FILE exists, at most SECS     "ok|timeout"
 *                     (e.g. Zero Ichi's "camera ready" - the drivers the
 *                     camera doesn't need load after it, not while it starts)
 *
 * --direct: drive the SPI hardware itself (through /dev/mem) - no need to wait
 * for the kernel's SPI driver to load, so the picture shows about a second
 * sooner. (Exit 3: not possible here - use the normal way.)
 * --update: the screen is set up already (Zero Ichi ran): no reset, no waits -
 * the picture just changes (used for the card image's progress).
 * --progress N: a progress bar at N % (0-100) with "N%" under it, drawn over
 * the picture: 3/5 down, across the middle 4/5 (Zero Ichi leaves that free).
 * Several IMAGEs: the one that fits the screen's size is shown.
 * Each Pi remembers its own screen: "lcd-SERIAL.conf" next to CONFIG (SERIAL =
 * this Pi's serial number), when there is one, is used instead of CONFIG -
 * so a card moved between Zero Ichi cameras with different screens shows the right
 * picture on each.
 *
 * CONFIG: "key value" lines written by Zero Ichi (/var/lib/zeroichi/lcd.conf):
 *   controller st7789 | st7789-2.8inch | ili9341   ("st7789-...": the 2.8inch hat)
 *   width, height, madctl, invert (0/1), dc, rst, bl (BCM pins), cs (0/1),
 *   speed (SPI Hz), flip (0/1 = turned 180 degrees)
 * IMAGE: width x height pixels, 16-bit big-endian RGB565 (Zero Ichi's splash files)
 *
 * The panel set-up is the same as Zero Ichi's lcd.py. GPIO pins are set through
 * /dev/gpiomem (or /dev/mem): unlike a GPIO "request", these settings stay
 * after the program ends, so the backlight stays on.
 *
 * Build (static, for the Pi Zero's ARMv6):
 *   zig cc -target arm-linux-musleabihf -mcpu=arm1176jzf_s -Os -static -s \
 *          -o zeroichi-lcd zeroichi-lcd.c
 * Test hooks: ZEROICHI_LCD_GPIOMEM / ZEROICHI_LCD_SPIDEV (other paths),
 * ZEROICHI_LCD_TRACE=1 (print every command and data length to stderr),
 * ZEROICHI_LCD_SYSBLOCK / ZEROICHI_LCD_DEVDIR (--partuuid), ZEROICHI_LCD_DRYRUN (--clock).
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define SPI_IOC_WR_MODE          0x40016b01u
#define SPI_IOC_WR_BITS_PER_WORD 0x40016b03u
#define SPI_IOC_WR_MAX_SPEED_HZ  0x40046b04u
#define GPIO_PHYS                0x20200000u      /* BCM2835 (Pi Zero / Pi 1) */

static volatile uint32_t *gpio, *spi0;          /* (spi0: --direct) */
static int spi = -1, dc_pin, trace, spi0_cs;
static size_t chunk = 4096;

static void msleep(int ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
}

static void pin_output(int pin)
{
    int reg = pin / 10, shift = (pin % 10) * 3;
    gpio[reg] = (gpio[reg] & ~(7u << shift)) | (1u << shift);
}

static void pin_write(int pin, int high)
{
    gpio[high ? 7 : 10] = 1u << pin;                /* GPSET0 / GPCLR0 */
}

/* --direct: the SPI0 block's registers, polled (CS, FIFO, CLK) */
#define SPI_CS_CLEAR 0x30u
#define SPI_CS_TA    0x80u
#define SPI_CS_DONE  0x10000u
#define SPI_CS_RXD   0x20000u
#define SPI_CS_TXD   0x40000u

static int spi0_write(const uint8_t *p, size_t n)
{
    long guard = 0;
    spi0[0] = (uint32_t)spi0_cs | SPI_CS_CLEAR;
    spi0[0] = (uint32_t)spi0_cs | SPI_CS_TA;
    size_t i = 0;
    while (i < n) {
        uint32_t cs = spi0[0];
        if (cs & SPI_CS_TXD)
            spi0[1] = p[i++];
        if (cs & SPI_CS_RXD)
            (void)spi0[1];                          /* (nothing comes back: keep it moving) */
        if (++guard > 200000000L)
            return -1;
    }
    while (!(spi0[0] & SPI_CS_DONE)) {
        if (spi0[0] & SPI_CS_RXD)
            (void)spi0[1];
        if (++guard > 200000000L)
            return -1;
    }
    while (spi0[0] & SPI_CS_RXD)
        (void)spi0[1];
    spi0[0] = (uint32_t)spi0_cs;
    return 0;
}

static int spi_write(const uint8_t *p, size_t n)
{
    if (spi0)
        return spi0_write(p, n);
    while (n) {
        size_t k = n < chunk ? n : chunk;
        ssize_t w = write(spi, p, k);
        if (w <= 0) {
            if (w < 0 && errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        n -= (size_t)w;
    }
    return 0;
}

static int cmd(uint8_t c, const uint8_t *data, size_t n)
{
    if (trace)
        fprintf(stderr, "C %02x %zu\n", c, n);
    pin_write(dc_pin, 0);
    if (spi_write(&c, 1))
        return -1;
    if (n) {
        pin_write(dc_pin, 1);
        if (trace) {
            for (size_t i = 0; i < n && i < 16; i++)
                fprintf(stderr, "D %02x\n", data[i]);
        }
        if (spi_write(data, n))
            return -1;
    }
    return 0;
}

#define CMD(c, ...) do { static const uint8_t d_[] = { __VA_ARGS__ }; \
                         if (cmd(c, d_, sizeof d_)) return -1; } while (0)
#define CMD0(c) do { if (cmd(c, NULL, 0)) return -1; } while (0)

static int init_st7789(void)                       /* Waveshare reference code */
{
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
    CMD(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B,
        0x1F, 0x23);
    CMD(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F,
        0x20, 0x23);
    return 0;
}

/* (waits: the panel needs 5 ms after "sleep out" (0x11) before the next
 * command - its maker's driver waits 120 ms there and after "display on",
 * which only matters for what's on the screen meanwhile: the backlight
 * stays off until the picture is written. So 10 ms, and none after.) */
static int init_hat28(void)                        /* the 2.8inch hat's panel */
{
    CMD0(0x11);
    msleep(10);
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
    CMD(0xE0, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0E, 0x12,
        0x14, 0x17);
    CMD(0xE1, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17,
        0x1B, 0x1E);
    CMD0(0x21);
    CMD0(0x29);
    return 0;
}

static int init_ili9341(void)
{
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
    CMD(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1, 0x37, 0x07, 0x10, 0x03,
        0x0E, 0x09, 0x00);
    CMD(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1, 0x48, 0x08, 0x0F, 0x0C,
        0x31, 0x36, 0x0F);
    return 0;
}

struct conf {
    char controller[64];
    long width, height, madctl, invert, dc, rst, bl, cs, speed, flip;
};

static int read_conf(const char *path, struct conf *c)
{
    char line[160], key[40], val[60];
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    memset(c, 0, sizeof *c);
    strcpy(c->controller, "st7789");
    c->width = c->height = 240;
    c->madctl = 0x70;
    c->invert = 1;
    c->dc = 25;
    c->rst = 27;
    c->bl = 24;
    c->speed = 40000000;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%39s %59s", key, val) != 2)
            continue;
        long v = strtol(val, NULL, 0);
        if (!strcmp(key, "controller")) snprintf(c->controller, sizeof c->controller, "%s", val);
        else if (!strcmp(key, "width")) c->width = v;
        else if (!strcmp(key, "height")) c->height = v;
        else if (!strcmp(key, "madctl")) c->madctl = v;
        else if (!strcmp(key, "invert")) c->invert = v;
        else if (!strcmp(key, "dc")) c->dc = v;
        else if (!strcmp(key, "rst")) c->rst = v;
        else if (!strcmp(key, "bl")) c->bl = v;
        else if (!strcmp(key, "cs")) c->cs = v;
        else if (!strcmp(key, "speed")) c->speed = v;
        else if (!strcmp(key, "flip")) c->flip = v;
    }
    fclose(f);
    if (c->width < 16 || c->width > 480 || c->height < 16 || c->height > 480)
        return -1;
    if (c->dc < 0 || c->dc > 27 || c->rst < 0 || c->rst > 27 || c->bl < 0 || c->bl > 27)
        return -1;
    if (c->speed < 1000000 || c->speed > 80000000)
        c->speed = 40000000;
    return 0;
}

static int open_gpio(void)
{
    const char *p = getenv("ZEROICHI_LCD_GPIOMEM");
    int fd = open(p ? p : "/dev/gpiomem", O_RDWR | O_SYNC);
    off_t off = 0;
    if (fd < 0 && !p) {
        fd = open("/dev/mem", O_RDWR | O_SYNC);
        off = GPIO_PHYS;
    }
    if (fd < 0)
        return -1;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    gpio = (volatile uint32_t *)m;
    return 0;
}

/* the Pi's peripherals' address (Pi Zero / 1: 0x20000000, Zero 2 / 2 / 3: 0x3F000000) */
static uint32_t peripheral_base(void)
{
    unsigned char b[12];
    uint32_t base = 0;
    FILE *f = fopen("/proc/device-tree/soc/ranges", "rb");
    if (f) {
        if (fread(b, 1, 12, f) == 12) {
            base = (uint32_t)b[4] << 24 | (uint32_t)b[5] << 16 | (uint32_t)b[6] << 8 | b[7];
            if (!base)
                base = (uint32_t)b[8] << 24 | (uint32_t)b[9] << 16 | (uint32_t)b[10] << 8 | b[11];
        }
        fclose(f);
    }
    return base ? base : 0x20000000u;
}

static void pin_alt0(int pin)
{
    int reg = pin / 10, shift = (pin % 10) * 3;
    gpio[reg] = (gpio[reg] & ~(7u << shift)) | (4u << shift);
}

static int open_spi0(int cs, long speed)
{
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(peripheral_base() + 0x204000u));
    close(fd);
    if (m == MAP_FAILED)
        return -1;
    spi0 = (volatile uint32_t *)m;
    pin_alt0(cs ? 7 : 8);                           /* CE, MISO, MOSI, SCLK */
    pin_alt0(9);
    pin_alt0(10);
    pin_alt0(11);
    spi0_cs = cs ? 1 : 0;                           /* mode 0, chip select active low */
    spi0[0] = (uint32_t)spi0_cs | SPI_CS_CLEAR;
    /* the clock: the core clock (Pi Zero: up to 400 MHz) / an even number, at
     * most the screen's speed - 40 MHz: 400/10 (25 MHz while the core runs at
     * 250) - as Linux's SPI driver would set it */
    long div = (400000000L + speed - 1) / speed;
    div += div & 1;
    spi0[2] = (uint32_t)(div < 4 ? 4 : div > 64 ? 64 : div);
    return 0;
}

/* a button: input with pull-up, held = low (three looks, 5 ms apart) */
static int held(int pin)
{
    if (pin < 0 || pin > 27 || open_gpio())
        return 2;
    int reg = pin / 10, shift = (pin % 10) * 3;
    gpio[reg] &= ~(7u << shift);                    /* input */
    gpio[37] = 2;                                   /* GPPUD: pull-up ... */
    for (volatile int i = 0; i < 1000; i++) {}
    gpio[38] = 1u << pin;                           /* ... clocked into this pin */
    for (volatile int i = 0; i < 1000; i++) {}
    gpio[37] = 0;
    gpio[38] = 0;
    int low = 0;
    for (int k = 0; k < 3; k++) {
        msleep(5);
        if (!(gpio[13] >> pin & 1u))                /* GPLEV0 */
            low++;
    }
    return low == 3 ? 0 : 1;
}

/* this Pi's own screen settings, if it has some (lcd-SERIAL.conf next to path) */
static const char *own_conf(const char *path, char *out, size_t cap)
{
    char serial[40] = "";
    FILE *f = fopen("/proc/device-tree/serial-number", "rb");
    if (f) {
        size_t n = fread(serial, 1, sizeof serial - 1, f);
        fclose(f);
        serial[n] = 0;
    }
    char clean[40];
    size_t j = 0;
    for (size_t i = 0; serial[i] && j < sizeof clean - 1; i++)
        if ((serial[i] >= '0' && serial[i] <= '9') || (serial[i] >= 'a' && serial[i] <= 'f') ||
            (serial[i] >= 'A' && serial[i] <= 'F'))
            clean[j++] = serial[i];
    clean[j] = 0;
    if (!j)
        return path;
    const char *sl = strrchr(path, '/');
    int dl = sl ? (int)(sl - path) : 1;
    snprintf(out, cap, "%.*s/lcd-%s.conf", dl, sl ? path : ".", clean);
    return access(out, R_OK) == 0 ? out : path;
}

static void read_bufsiz(void)
{
    FILE *f = fopen("/sys/module/spidev/parameters/bufsiz", "r");
    long v = 0;
    if (f) {
        if (fscanf(f, "%ld", &v) != 1)
            v = 0;
        fclose(f);
    }
    if (v >= 4096 && v <= (1 << 20))
        chunk = (size_t)v;
}

/* "0".."9", "%": 5x7 dots, one byte per row (bit 4 = left) */
static const uint8_t DIGITS[11][7] = {
    {14, 17, 19, 21, 25, 17, 14}, {4, 12, 4, 4, 4, 4, 14}, {14, 17, 1, 2, 4, 8, 31},
    {31, 2, 4, 2, 1, 17, 14}, {2, 6, 10, 18, 31, 2, 2}, {31, 16, 30, 1, 1, 17, 14},
    {6, 8, 16, 30, 17, 17, 14}, {31, 1, 2, 4, 8, 8, 8}, {14, 17, 17, 14, 17, 17, 14},
    {14, 17, 17, 15, 1, 2, 12}, {24, 25, 2, 4, 8, 19, 3}};

static void put_px(uint8_t *img, long w, long h, long x, long y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return;
    img[(y * w + x) * 2] = (uint8_t)(c >> 8);
    img[(y * w + x) * 2 + 1] = (uint8_t)c;
}

static void fill(uint8_t *img, long w, long h, long x0, long y0, long x1, long y1, uint16_t c)
{
    for (long y = y0; y < y1; y++)
        for (long x = x0; x < x1; x++)
            put_px(img, w, h, x, y, c);
}

/* the progress bar (in the picture as it stands upright) */
static void draw_progress(uint8_t *img, long w, long h, int pct)
{
    const uint16_t WHITE = 0xFFFF, YELLOW = 0xFEA0, DARK = 0x2124, BG = 0x10A3;   /* (BG: Zero Ichi's screens) */
    if (pct < 0)
        pct = 0;
    if (pct > 100)
        pct = 100;
    long x0 = w / 10, x1 = w - w / 10, y0 = h * 3 / 5, y1 = y0 + 18;
    fill(img, w, h, x0 - 2, y0 - 2, x1 + 2, y1 + 2, WHITE);           /* frame */
    fill(img, w, h, x0, y0, x1, y1, DARK);
    fill(img, w, h, x0, y0, x0 + (x1 - x0) * pct / 100, y1, YELLOW);
    char t[8];
    int n = snprintf(t, sizeof t, "%d%%", pct), sc = 2;
    long tx = (w - n * 6 * sc) / 2, ty = y1 + 10;
    fill(img, w, h, tx - 8, ty - 3, tx + n * 6 * sc + 6, ty + 7 * sc + 3, BG);
    for (int i = 0; i < n; i++) {
        const uint8_t *g = DIGITS[t[i] == '%' ? 10 : t[i] - '0'];
        for (int r = 0; r < 7; r++)
            for (int col = 0; col < 5; col++)
                if (g[r] >> (4 - col) & 1)
                    fill(img, w, h, tx + (i * 6 + col) * sc, ty + r * sc, tx + (i * 6 + col + 1) * sc,
                         ty + (r + 1) * sc, WHITE);
    }
}

/* ------------------------------------------------- (v52) start-up jobs */
static int mnt(const char *src, const char *dir, const char *type, unsigned long flags, const char *data)
{
    if (mount(src, dir, type, flags, data) == 0 || errno == EBUSY)
        return 0;
    return -1;
}

static int early(int pin)
{
    int bad = mnt("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    bad |= mnt("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    if (access("/dev/mmcblk0", F_OK) != 0)          /* (the kernel normally has /dev ready) */
        bad |= mnt("devtmpfs", "/dev", "devtmpfs", MS_NOSUID, "mode=0755");
    int k = held(pin);
    printf("%s %s\n", bad ? "fail" : "ok", k == 0 ? "held" : k == 1 ? "free" : "unknown");
    return 0;
}

static int mount_run(void)
{
    int bad = mnt("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
    const char *dirs[] = {"/run/lock", "/run/zeroichi", "/dev/pts", "/dev/shm"};
    for (unsigned i = 0; i < sizeof dirs / sizeof dirs[0]; i++)
        if (mkdir(dirs[i], 0755) && errno != EEXIST)
            bad = 1;
    bad |= mnt("devpts", "/dev/pts", "devpts", 0, "gid=5,mode=620");
    bad |= mnt("tmpfs", "/dev/shm", "tmpfs", MS_NOSUID | MS_NODEV, NULL);
    bad |= mnt("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
    char h[65] = "";
    FILE *f = fopen("/etc/hostname", "r");
    if (f) {
        if (fgets(h, sizeof h, f)) {
            h[strcspn(h, " \t\r\n")] = 0;
            if (h[0] && sethostname(h, strlen(h)))
                bad = 1;
        }
        fclose(f);
    }
    printf("%s\n", bad ? "fail" : "ok");
    return 0;
}

static int set_clock(int n, char **files)
{
    time_t now = time(NULL), best = now;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(files[i]);
        if (l >= 17 && !strcmp(files[i] + l - 17, "fake-hwclock.data")) {
            FILE *f = fopen(files[i], "r");               /* "YYYY-MM-DD HH:MM:SS" (UTC) */
            struct tm tm = {0};
            if (f && fscanf(f, "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min,
                            &tm.tm_sec) == 6) {
                tm.tm_year -= 1900;
                tm.tm_mon -= 1;
                time_t t = timegm(&tm);
                if (t > best)
                    best = t;
            }
            if (f)
                fclose(f);
            continue;
        }
        struct stat st;
        if (stat(files[i], &st) == 0 && st.st_mtime > best)
            best = st.st_mtime;
    }
    if (getenv("ZEROICHI_LCD_DRYRUN")) {                 /* (tests: say it, don't set it) */
        printf("ok %lld\n", (long long)best);
        return 0;
    }
    if (best > now) {
        struct timeval tv = {best, 0};
        settimeofday(&tv, NULL);
    }
    printf("ok\n");
    return 0;
}

static int fat_dirty(const char *dev)
{
    unsigned char b[512];
    int fd = open(dev, O_RDONLY);
    if (fd < 0 || read(fd, b, sizeof b) != (ssize_t)sizeof b) {
        if (fd >= 0)
            close(fd);
        return 2;
    }
    close(fd);
    int at = memcmp(b + 82, "FAT32", 5) == 0 ? 65 : 37;   /* (the "not closed" flag: FAT32 / FAT16) */
    printf("%s\n", b[at] & 1 ? "dirty" : "clean");
    return 0;
}

static int partuuid(const char *spec)
{
    char sig[16];
    unsigned part;
    if (strncmp(spec, "PARTUUID=", 9) || sscanf(spec + 9, "%8[0-9a-fA-F]-%x", sig, &part) != 2 || strlen(sig) != 8)
        return 2;
    for (char *p = sig; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    const char *sb = getenv("ZEROICHI_LCD_SYSBLOCK"), *dv = getenv("ZEROICHI_LCD_DEVDIR");   /* (tests) */
    DIR *d = opendir(sb ? sb : "/sys/block");
    if (!d)
        return 2;
    struct dirent *e;
    int found = 2;
    while ((e = readdir(d)) && found) {
        if (strncmp(e->d_name, "mmcblk", 6) && strncmp(e->d_name, "sd", 2))
            continue;
        char dev[300];
        snprintf(dev, sizeof dev, "%s/%s", dv ? dv : "/dev", e->d_name);
        int fd = open(dev, O_RDONLY);
        unsigned char b[512];
        if (fd < 0)
            continue;
        ssize_t r = read(fd, b, sizeof b);
        close(fd);
        if (r != (ssize_t)sizeof b || b[510] != 0x55 || b[511] != 0xAA)
            continue;
        char got[16];
        snprintf(got, sizeof got, "%02x%02x%02x%02x", b[443], b[442], b[441], b[440]);   /* (MBR disk id) */
        if (strcmp(got, sig))
            continue;
        int digit = isdigit((unsigned char)e->d_name[strlen(e->d_name) - 1]);
        printf("/dev/%s%s%u\n", e->d_name, digit ? "p" : "", part);
        found = 0;
    }
    closedir(d);
    return found;
}

/* (v53) --wait: until the file is there (checked 20 times a second - no work
 * in between), at most `secs` seconds */
static int wait_file(const char *path, double secs)
{
    struct timespec t = {0, 50 * 1000 * 1000};
    for (int i = 0; i <= (int)(secs * 20); i++) {
        if (access(path, F_OK) == 0) {
            puts("ok");
            return 0;
        }
        nanosleep(&t, NULL);
    }
    puts("timeout");
    return 1;
}

int main(int argc, char **argv)
{
    struct conf c;
    int direct = 0, update = 0, progress = -1;
    if (argc == 3 && !strcmp(argv[1], "--held"))
        return held(atoi(argv[2]));
    if (argc == 3 && !strcmp(argv[1], "--early"))
        return early(atoi(argv[2]));
    if (argc == 2 && !strcmp(argv[1], "--mount-run"))
        return mount_run();
    if (argc >= 2 && !strcmp(argv[1], "--clock"))
        return set_clock(argc - 2, argv + 2);
    if (argc == 3 && !strcmp(argv[1], "--fat-dirty"))
        return fat_dirty(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "--partuuid"))
        return partuuid(argv[2]);
    if (argc == 4 && !strcmp(argv[1], "--wait"))
        return wait_file(argv[2], atof(argv[3]));
    for (;;) {
        if (argc > 1 && !strcmp(argv[1], "--direct")) {
            direct = 1;
        } else if (argc > 1 && !strcmp(argv[1], "--update")) {
            update = 1;
        } else if (argc > 2 && !strcmp(argv[1], "--progress")) {
            progress = atoi(argv[2]);
            argv++;
            argc--;
        } else {
            break;
        }
        argv++;
        argc--;
    }
    if (argc < 3) {
        fprintf(stderr, "usage: zeroichi-lcd [--direct] [--update] [--progress N] CONFIG IMAGE [IMAGE...]"
                        " | --held PIN\n");
        return 2;
    }
    trace = getenv("ZEROICHI_LCD_TRACE") != NULL;
    char ownp[512];
    const char *cpath = own_conf(argv[1], ownp, sizeof ownp);
    if (read_conf(cpath, &c)) {
        fprintf(stderr, "zeroichi-lcd: bad config %s\n", cpath);
        return 1;
    }
    size_t n = (size_t)(c.width * c.height * 2);
    uint8_t *img = malloc(n + 1);
    size_t got = 0;
    for (int a = 2; a < argc && got != n; a++) {   /* the picture that fits this screen */
        int fd = open(argv[a], O_RDONLY);
        if (!img || fd < 0)
            continue;
        got = 0;
        for (;;) {
            ssize_t r = read(fd, img + got, n + 1 - got);
            if (r <= 0)
                break;
            got += (size_t)r;
            if (got > n)
                break;
        }
        close(fd);
    }
    if (got != n) {
        fprintf(stderr, "zeroichi-lcd: no %ldx%ld picture\n", c.width, c.height);
        return 1;
    }
    if (progress >= 0)
        draw_progress(img, c.width, c.height, progress);
    if (c.flip) {                                   /* turned 180: reverse the pixels */
        for (size_t i = 0, j = n - 2; i < j; i += 2, j -= 2) {
            uint8_t a = img[i], b = img[i + 1];
            img[i] = img[j];
            img[i + 1] = img[j + 1];
            img[j] = a;
            img[j + 1] = b;
        }
    }
    if (open_gpio()) {
        fprintf(stderr, "zeroichi-lcd: no GPIO access\n");
        return 1;
    }
    if (direct && open_spi0((int)c.cs, c.speed)) {
        fprintf(stderr, "zeroichi-lcd: no direct SPI access\n");
        return 3;
    }
    char dev[64];
    const char *sp = getenv("ZEROICHI_LCD_SPIDEV");
    snprintf(dev, sizeof dev, "%s", sp ? sp : (c.cs ? "/dev/spidev0.1" : "/dev/spidev0.0"));
    if (!direct) {
        for (int i = 0; i < 150 && access(dev, F_OK) != 0; i++)
            msleep(10);                             /* (driver still loading) */
        spi = open(dev, O_RDWR);
        if (spi < 0) {
            fprintf(stderr, "zeroichi-lcd: can't open %s\n", dev);
            return 1;
        }
        uint8_t mode = 0, bits = 8;
        uint32_t speed = (uint32_t)c.speed;
        if ((ioctl(spi, SPI_IOC_WR_MODE, &mode) < 0 || ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0
             || ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &speed) < 0) && !sp) {
            fprintf(stderr, "zeroichi-lcd: SPI set-up failed\n");
            return 1;
        }
        read_bufsiz();
    }
    dc_pin = (int)c.dc;
    pin_output((int)c.dc);
    pin_output((int)c.rst);
    pin_output((int)c.bl);
    int bad = 0;
    if (update) {                                   /* set up already: just the essentials */
        uint8_t md = (uint8_t)c.madctl, cm = 0x55;
        bad = cmd(0x11, NULL, 0);                   /* (awake already: no effect) */
        msleep(10);
        bad = bad || cmd(0x36, &md, 1) || cmd(c.invert ? 0x21 : 0x20, NULL, 0) || cmd(0x3A, &cm, 1)
              || cmd(0x29, NULL, 0);
        goto picture;
    }
    pin_write((int)c.rst, 1);                       /* reset the panel */
    msleep(10);
    pin_write((int)c.rst, 0);
    msleep(10);
    pin_write((int)c.rst, 1);
    msleep(120);
    if (!strcmp(c.controller, "ili9341"))
        bad = init_ili9341();
    else if (!strncmp(c.controller, "st7789-", 7))
        bad = init_hat28() || init_hat28();         /* twice, as its maker's driver does */
    else
        bad = init_st7789();
    uint8_t md = (uint8_t)c.madctl;
    bad = bad || cmd(0x36, &md, 1) || cmd(c.invert ? 0x21 : 0x20, NULL, 0);
    if (!bad && strncmp(c.controller, "st7789-", 7)) {
        bad = cmd(0x11, NULL, 0);                   /* sleep out (5 ms needed, see init_hat28) */
        msleep(10);
        bad = bad || cmd(0x29, NULL, 0);            /* display on */
    }
picture:;
    uint8_t xs[4] = { 0, 0, (uint8_t)((c.width - 1) >> 8), (uint8_t)((c.width - 1) & 0xFF) };
    uint8_t ys[4] = { 0, 0, (uint8_t)((c.height - 1) >> 8), (uint8_t)((c.height - 1) & 0xFF) };
    bad = bad || cmd(0x2A, xs, 4) || cmd(0x2B, ys, 4) || cmd(0x2C, img, n);
    pin_write((int)c.bl, 1);                        /* backlight on */
    if (spi >= 0)
        close(spi);
    if (bad) {
        fprintf(stderr, "zeroichi-lcd: writing to the screen failed\n");
        return 1;
    }
    return 0;
}
