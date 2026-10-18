/* Zero Ichi - GPIO pins through /dev/gpiomem (BCM2835: Pi Zero / Pi 1) and the
 * buttons (a small thread that watches them and reports presses and holds). */
#ifndef ZEROICHI_GPIO_H
#define ZEROICHI_GPIO_H

int gpio_open(void);                  /* 0 = ok */
void gpio_output(int pin);
void gpio_write(int pin, int high);
void gpio_input_pullup(int pin);
int gpio_read(int pin);               /* 1 = high */
unsigned gpio_levels(void);           /* all of pins 0-31 */

enum { BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_PRESS, BTN_KEY1, BTN_KEY2, BTN_KEY3, NBTN };
enum { EV_PRESS = 1, EV_HOLD = 2 };
extern const char *const BTN_NAMES[NBTN];

/* starts the button thread; events can be read from the returned fd (pipe),
 * 2 bytes each: button, kind */
int buttons_start(const int pins[NBTN], int hold_button, double hold_seconds);
void buttons_stop(void);
int button_held_now(int pin);         /* at start-up: is this button held down? */
/* tests: buttons come from this file descriptor instead ("key1 press" lines) */
void buttons_inject_fd(int fd);
#endif
