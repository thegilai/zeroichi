#include "gpio.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "util.h"

#define GPIO_PHYS 0x20200000u
static volatile uint32_t *gpio;
static int sim;

const char *const BTN_NAMES[NBTN] = {"up", "down", "left", "right", "press", "key1", "key2", "key3"};

int gpio_open(void) {
    if (gpio) return 0;
    if (getenv("ZEROICHI_SIM")) {
        sim = 1;
        gpio = calloc(1024, 4);
        for (int i = 0; i < 32; i++) gpio[13] |= 1u << i;      /* (levels: all high = released) */
        return 0;
    }
    int fd = open("/dev/gpiomem", O_RDWR | O_SYNC);
    off_t off = 0;
    if (fd < 0) {
        fd = open("/dev/mem", O_RDWR | O_SYNC);
        off = GPIO_PHYS;
    }
    if (fd < 0) return -1;
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, off);
    close(fd);
    if (m == MAP_FAILED) return -1;
    gpio = (volatile uint32_t *)m;
    return 0;
}

void gpio_output(int pin) {
    int reg = pin / 10, shift = (pin % 10) * 3;
    gpio[reg] = (gpio[reg] & ~(7u << shift)) | (1u << shift);
}

void gpio_write(int pin, int high) {
    if (sim) return;
    gpio[high ? 7 : 10] = 1u << pin;                 /* GPSET0 / GPCLR0 */
}

static void tiny_wait(void) {
    struct timespec t = {0, 20000};
    nanosleep(&t, NULL);
}

void gpio_input_pullup(int pin) {
    int reg = pin / 10, shift = (pin % 10) * 3;
    gpio[reg] = gpio[reg] & ~(7u << shift);          /* input */
    if (sim) return;
    gpio[37] = 2;                                    /* GPPUD: pull-up */
    tiny_wait();
    gpio[38] = 1u << pin;                            /* GPPUDCLK0 */
    tiny_wait();
    gpio[37] = 0;
    gpio[38] = 0;
}

unsigned gpio_levels(void) { return gpio[13]; }     /* GPLEV0 */
int gpio_read(int pin) { return (gpio[13] >> pin) & 1; }

int button_held_now(int pin) {
    if (gpio_open()) return 0;
    if (sim) return getenv("ZEROICHI_SIM_HOLD_KEY2") && pin == 20;
    gpio_input_pullup(pin);
    msleep(5);
    for (int i = 0; i < 3; i++) {
        if (gpio_read(pin)) return 0;
        msleep(10);
    }
    return 1;
}

/* ------------------------------------------------------------- the thread */
static int pins[NBTN], hold_btn = -1;
static double hold_time = 3.0;
static int pipe_w = -1, pipe_r = -1, inject_fd = -1;
static volatile int stop;
static pthread_t th;

void buttons_inject_fd(int fd) { inject_fd = fd; }

static void emit(int b, int kind) {
    unsigned char m[2] = {(unsigned char)b, (unsigned char)kind};
    if (write(pipe_w, m, 2) != 2) { /* (pipe full: drop) */ }
}

static unsigned read_pressed(void) {
    unsigned lv = gpio_levels(), bits = 0;
    for (int i = 0; i < NBTN; i++)
        if (!((lv >> pins[i]) & 1)) bits |= 1u << i;      /* pressed = pulled low */
    return bits;
}

static void *inject_loop(void *arg) {
    (void)arg;
    char buf[256];
    size_t have = 0;
    while (!stop) {                               /* ("key1 press" / "key3 hold" lines) */
        ssize_t n = read(inject_fd, buf + have, sizeof buf - 1 - have);
        if (n <= 0) break;
        have += (size_t)n;
        buf[have] = 0;
        char *nl;
        while ((nl = memchr(buf, '\n', have))) {
            *nl = 0;
            char name[32], kind[32] = "";
            if (sscanf(buf, "%31s %31s", name, kind) >= 1)
                for (int i = 0; i < NBTN; i++)
                    if (!strcmp(name, BTN_NAMES[i])) emit(i, !strcmp(kind, "hold") ? EV_HOLD : EV_PRESS);
            size_t used = (size_t)(nl - buf) + 1;
            memmove(buf, nl + 1, have - used);
            have -= used;
        }
        if (have >= sizeof buf - 1) have = 0;
    }
    return NULL;
}

static void *poll_loop(void *arg) {
    (void)arg;
    unsigned stable = read_pressed(), last = stable;
    /* buttons already held when Zero Ichi starts count only after a release */
    int held = hold_btn >= 0 && (stable >> hold_btn) & 1;
    unsigned ignore = stable;
    double down_at[NBTN] = {0};
    while (!stop) {
        double now = mono();
        unsigned cur = read_pressed();
        if (cur != last) {
            last = cur;                                  /* needs 2 equal reads (debounce) */
        } else if (cur != stable) {
            unsigned changed = cur ^ stable;
            for (int i = 0; i < NBTN; i++) {
                unsigned bit = 1u << i;
                if (!(changed & bit)) continue;
                if (cur & bit) {
                    down_at[i] = now;
                    ignore &= ~bit;
                    if (i != hold_btn) emit(i, EV_PRESS);
                } else if (i == hold_btn) {
                    if (held) held = 0;
                    else if (!(ignore & bit)) emit(i, EV_PRESS);
                } else {
                    ignore &= ~bit;
                }
            }
            stable = cur;
        }
        if (hold_btn >= 0 && (stable >> hold_btn) & 1 && !held && !(ignore >> hold_btn & 1) &&
            now - down_at[hold_btn] >= hold_time) {
            held = 1;
            emit(hold_btn, EV_HOLD);
        }
        msleep(20);
    }
    return NULL;
}

int buttons_start(const int p[NBTN], int hold_button, double hold_seconds) {
    memcpy(pins, p, sizeof pins);
    hold_btn = hold_button;
    hold_time = hold_seconds;
    int fds[2];
    if (pipe(fds)) return -1;
    pipe_r = fds[0];
    pipe_w = fds[1];
    fcntl(pipe_r, F_SETFL, O_NONBLOCK);
    fcntl(pipe_w, F_SETFL, O_NONBLOCK);
    fcntl(pipe_r, F_SETFD, FD_CLOEXEC);
    fcntl(pipe_w, F_SETFD, FD_CLOEXEC);
    if (gpio_open()) return pipe_r;
    for (int i = 0; i < NBTN; i++) gpio_input_pullup(pins[i]);
    pthread_create(&th, NULL, inject_fd >= 0 ? inject_loop : poll_loop, NULL);
    return pipe_r;
}

void buttons_stop(void) { stop = 1; }
