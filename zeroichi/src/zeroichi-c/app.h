/* Zero Ichi - the camera app (a port of the Python Zero Ichi's app.py) */
#ifndef ZEROICHI_APP_H
#define ZEROICHI_APP_H
#include <pthread.h>
#include <stdint.h>
#include "camera.h"
#include "canvas.h"
#include "img.h"
#include "jpeg.h"
#include "looks.h"
#include "mov.h"
#include "state.h"
#include "venc.h"

#include "meta.h"                     /* (VERSION) */
#define JPEG_QUALITY 90
#define VIDEO_QUALITY 75
#define VIDEO_FPS 15
#define PHOTO_W 1296                   /* the photo stream: the sensor's 2x2-binned mode */
#define PHOTO_H 972
#define MIN_SPACE (30LL << 20)
#define LOW_SPACE (200LL << 20)
#define MAX_VIDEO_KB (3500u << 10)        /* one video file (the FAT drive's limit is 4 GB) */
#define HOLD_SECONDS 3.0
#define MENU_ROWS 7

/* exit codes for the fast start */
#define EXIT_POWEROFF 64
#define EXIT_NORMAL_START 65
#define EXIT_REBOOT 66
#define EXIT_MAKE_SPACE 67
#define EXIT_MAKE_IMAGE 70
#define EXIT_UPDATE 68
#define EXIT_USE_PYTHON 69

enum { SCR_CAMERA, SCR_MENU, SCR_GALLERY, SCR_CONFIRM, SCR_DEVELOPING, SCR_SPACE, SCR_ABOUT, SCR_ERASE };
enum { MODE_PHOTO, MODE_GIF, MODE_VIDEO };

/* a small queue of jobs for a background thread */
typedef struct Job { void (*fn)(void *); void *arg; struct Job *next; } Job;
typedef struct {
    pthread_t th;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    Job *head, *tail;
    volatile int pending;              /* jobs waiting or running */
    int nice;
    const char *name;
} Worker;
void worker_start(Worker *w, const char *name, int nice);
void worker_submit(Worker *w, void (*fn)(void *), void *arg);
void worker_wait(Worker *w, double timeout);

/* results from background threads (shown by the main loop) */
typedef struct { char toast[128]; double seconds; int counted; char learn_kind[8]; int learn_look;
                 double pixels, bytes; char done_path[512]; } Result;

typedef struct Rec Rec;

/* what the live view made of a frame (kept for instant videos) */
typedef struct { int kind; Tone tone; Img rgb; Img y, u, v; } LiveItem;   /* kind 1 tone, 2 RGB, 3 Y,U,V */
void live_item_free(LiveItem *it);

typedef struct {
    Settings S;
    State st, saved;
    double dirty_at;
    int W, H;
    Canvas scr;
    char data_dir[512], copy_dir[512], state_file[512];
    char photo_dir[512], photo_setting[256];
    int screen, running;
    int menu_idx, menu_top;
    int presets_open, preset_idx;      /* MENU > Presets (a panel inside the menu) */
    char toast[160];
    double toast_until, toast_secs;
    double peek_until;              /* (v54) the MENU steps aside: the picture, while Rotation / Mirror changes */
    int splash_hold;                   /* the start-up picture stays until the live view is ready */
    double splash_since;
    int count;                         /* files in the photo folder */
    int asleep;                        /* 0 = awake, 1 = screen off (standby, or a long screen
                                        * saver), 2 = the screen saver's picture (v51) */
    double last_input;                 /* the last key press - or the end of a recording (v51) */
    double saver_t0, saver_next;       /* the screen saver: since when, its next picture */
    double ready_after;
    /* recording */
    Rec *rec;
    int hw_video_failed;
    double pace_start;
    /* camera */
    int cam_missing;
    double cam_retry_at;
    int cam_h, cam_v, cam_turn;        /* flips by the sensor, 90-degree turn in software */
    int cfg_key[10];                   /* the camera set-up in use */
    /* background work */
    Worker saver, developer;
    pthread_mutex_t res_lock;
    Result results[16];
    int nresults;
    volatile double dev_progress;      /* -1 = not developing */
    Img dev_preview;
    pthread_mutex_t dev_lock;
    double dev_shown;
    /* numbering */
    int next_num;
    char reserved[8][32];
    /* gallery */
    char **media;
    int nmedia, gidx;
    void *gal;
    /* space left */
    long long free_b;
    double free_at;
    /* live view performance */
    int lv_n;
    double lv_cam, lv_filter, lv_screen, lv_t0, lv_busy, lv_cpu, lv_last;
    double clock_at;
    double space_drawn;
    void *space_offer;
    double cpu_t0;
    int boot_marks_n;
    char boot_marks[16][32];
    double boot_mark_t[16];
} App;

extern App A;

/* app_view.c */
double picture_aspect(void);
void target_size(const char *kind, int *w, int *h);
void flips_for(int rot, int mirror, int *h, int *v, int *turn);
void stream_plan(int *mw, int *mh, int *lw, int *lh, int *photo);
void apply_camera(int force);
void draw_live(const CamFrame *f, void *rec_item_out);
void tick_camera_frame(const CamFrame *f);     /* (NULL: no new frame) */
void camera_step(void);                         /* the newest frame(s): view, recording */
void menu_refresh(void);                        /* (rows shown: worked out when the menu opens) */
int menu_count(void);
const char *menu_key(int i);
int menu_is_action(int i);
void draw_overlays(double now);
void draw_sd_dot(void);                 /* (the SD card being written: don't unplug) */
void show_message(const char *title, const char *sub, Rgb bg);   /* (v52: bg - red: a warning) */
void brand_lines(const char *const *lines, int n, Rgb title_col);  /* (v52: Zero Ichi's design) */
void show_toast(const char *text, double seconds);
void splash_screen(void);
void about_draw(void);                                       /* (v53) MENU > About */
void live_rate(double work, int exposure_us);                /* (v53) the live view's speed */
void live_rate_slow(void);
double rec_writer_cost(int look);                            /* (v54) a video's writer, per picture (s) */
void screen_lines(const char *const *lines, int n, Rgb title_color, Rgb bg);
void free_space_update(int force);
void instant_size(int look, int *w, int *h);
int instant_video(int look);
const char *mode_name(void);
int mode(void);

/* app_media.c */
void list_media(void);
void new_media_path(const char *prefix, const char *ext, char *out, size_t cap);
void unreserve(const char *path);
void trigger(void);
void stop_recording(void);
void rec_frame(const CamFrame *f, double now, void *item);
int rec_active(void);
int rec_kind(void);                    /* 0 none, 'g' GIF, 'v' video */
int rec_hw(void);
int rec_live(void);
double rec_start(void);
double rec_progress(double now);
const char *rec_label(char *buf, size_t cap, double now);
void rec_extra_frame(const CamFrame *f);
void rec_note_frame_cpu(double seconds);
void resume_developing(void);
void develop_submit(const char *raw);
void pump_results(void);
void wait_for_saves(const char *why);
void learn_space(const char *kind, int look, double pixels, double bytes);
int card_full(void);
void add_result(const char *toast, double seconds, int counted, const char *kind, int look, double pixels,
                double bytes, const char *path);

/* app_gallery.c */
void enter_gallery(void);
void on_gallery(int btn);
void on_confirm(int btn);
void erase_all_ask(void);                    /* (0.1.0) MENU > Erase all */
void on_erase(int btn);
void tick_gallery(void);
void gallery_redraw(void);

/* app_space.c */
void choose_photo_dir(void);
void maybe_offer_space(void);
void maybe_make_image(const char *sd_dir);
void test_space_plan(void);
void photo_space_action(void);
void on_space(int btn);
void tick_space(void);
const char *photo_space_label(void);
int photo_space_needed(void);                   /* 0: the whole card is in use already */
void save_message_screen(void);

/* app_presets.c */
#define NPRESETS 4
void presets_refresh(void);
const char *presets_label(void);
const char *this_pi(void);              /* this Pi's serial number ("" if unknown) */
void presets_draw(void);
/* (v52) the menus' parts in Zero Ichi's design (app_view.c) */
void ui_card(const char *title, int up);
void ui_row(int y, const char *label, const char *value, int chosen, Rgb label_col, Rgb value_col);
void ui_hint(const char *text, int more);
int ui_card_reuse(const char *key);
void ui_card_keep(const char *key);
void presets_key(int btn);

/* app_main.c */
void mark_dirty(void);
void save_state(void);
void state_loaded(const State *old, const char *screen);     /* (after loading a preset) */
void power_off(void);
void cleanup(int keep_screen);
void remember_clock(void);
void enter_developing(void);
void tick_developing(void);
void drain_events(void);
int key_pressed(int btn);
void app_exit(int code);
const char *pi_health(char *buf, size_t cap);
void cpu_report(char *buf, size_t cap, int restart);
void release_frame(int cookie);
#endif
