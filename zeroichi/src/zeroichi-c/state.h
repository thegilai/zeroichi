/* Zero Ichi - settings.txt (start-up options), state.json (everything set in
 * the camera and its menu) and the presets (presets/preset-N.txt) - all in the
 * zeroichi folder on the card's ZEROICHI drive, nowhere else. The same files and
 * format as the Python version, so switching between the two keeps every
 * setting. */
#ifndef ZEROICHI_STATE_H
#define ZEROICHI_STATE_H
#include <stddef.h>

typedef struct {
    char photo_dir[256];
    int rotate, swap_rb, allow_shutdown, no_camera, diagnostics, optimize_boot, fast_start;
    int spi_speed, display_madctl, display_invert, spi_cs, display_bits;   /* -1 = auto */
    char display[32];
    int pin_dc, pin_rst, pin_bl, pin_key[8];                                 /* -1 = default */
    char app[16];                                                           /* "c" / "python" */
    char photo_space[16];                                                   /* "auto" / "menu" */
    int live_fps;                                                           /* 0 = default */
} Settings;

void settings_read(const char *path, Settings *s);

#define NSPACE 40
typedef struct {
    char mode[8];                 /* PHOTO / GIF / VIDEO */
    int filter;                   /* LOOK_... */
    double ev;
    int cam_rotate, mirror, screen_flip, gif_frames, video_max;
    char size[8];                 /* Screen / 4:3 / Square */
    char clip_size[12];           /* (v55) 320x240 / 640x480 */
    char screen[32];              /* "" or a display name */
    char screensaver[8];
    char space_key[NSPACE][40];   /* "photo:Normal" -> bytes per pixel */
    double space_val[NSPACE];
    int nspace;
    int next_number, space_asked;
    char key3[12];                /* "Power off" / "Standby" */
    /* each Zero Ichi's own screen: Pi serial number -> display name (a card moved
     * between Zero Ichi cameras with different screens finds the right one by itself) */
    int nscreens;
    char scr_id[8][24], scr_name[8][32];
} State;

void state_default(State *st, const Settings *s);
int state_load(State *st, const char *path);             /* 0 if it wasn't there */
int state_save(const State *st, const char *path);
void state_validate(State *st);
extern const char *const CLIP_NAMES[2];     /* (v55) the clip sizes: 320x240, 640x480 */
const char *ratio_label(const char *size);  /* (0.1.0) "Screen" -> "Fit screen", "Square" -> "1:1" */
/* a preset: every setting of the camera and its menu (look, mode, EV,
 * rotation, image ratio, mirror, flip screen, clip size, GIF frames, video max,
 * screen, screen saver, KEY3 hold) as "key = value" lines - easy to
 * read, edit and copy on a computer. screen: "320x240" / "240x240" ("" in
 * preset_read: not in the file). camera: the Pi serial of the Zero Ichi it was
 * saved on ("" in preset_read: not in the file - presets before v43) */
int preset_write(const State *st, const char *screen, const char *camera, const char *path, int number);
int preset_read(State *st, const char *path, char *screen, size_t cap, char *camera, size_t ccap);  /* 0 = read */
/* display names: "1.3inch-hat", "2.8inch-hat", ... (older and other spellings
 * are turned into these) */
void display_canon(char *name, size_t cap);
int state_equal(const State *a, const State *b);
double state_space(const State *st, const char *key);       /* -1 = not known */
const char *state_screen_of(const State *st, const char *pi);   /* NULL = not known */
void state_set_screen_of(State *st, const char *pi, const char *name);
void state_set_space(State *st, const char *key, double v);
#endif
