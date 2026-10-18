#define _GNU_SOURCE
/* Zero Ichi - start-up, the main loop, buttons, the menu, power and standby
 * (a port of app.py's Zero Ichi class and zeroichi.py's launcher) */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "app.h"
#include "font.h"
#include "gpio.h"
#include "jpeg.h"
#include "lcd.h"
#include "screensaver.h"
#include "util.h"

App A;
static int btn_fd = -1, flip_controls;
static double t_started;
static const char *SD_DIR = "/boot/firmware/zeroichi";
static char pi_serial[24];                         /* this Pi (each Zero Ichi's own screen) */
const char *this_pi(void) { return pi_serial; }

static void read_pi_serial(void) {
    const char *t = getenv("ZEROICHI_SERIAL");           /* (tests) */
    char *f = t ? NULL : read_file("/proc/device-tree/serial-number", NULL);
    const char *s = t ? t : f ? f : "";
    size_t j = 0;
    for (size_t i = 0; s[i] && j < sizeof pi_serial - 1; i++)
        if ((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f') || (s[i] >= 'A' && s[i] <= 'F'))
            pi_serial[j++] = s[i];
    pi_serial[j] = 0;
    free(f);
}

/* zeroichi-lcd's settings for this Pi: lcd-SERIAL.conf (or the shared lcd.conf) */
static void helper_conf_path(char *out, size_t cap, int own) {
    if (own && pi_serial[0]) snprintf(out, cap, "/var/lib/zeroichi/lcd-%s.conf", pi_serial);
    else snprintf(out, cap, "/var/lib/zeroichi/lcd.conf");
}
static char camd_path[600];
static int camd_spawned_at_fail;
static double cam_spawn_t;


/* ------------------------------------------------------------- workers */
static void *worker_loop(void *arg) {
    Worker *w = arg;
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), w->nice);
    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->head) pthread_cond_wait(&w->cond, &w->lock);
        Job *j = w->head;
        w->head = j->next;
        if (!w->head) w->tail = NULL;
        pthread_mutex_unlock(&w->lock);
        j->fn(j->arg);
        free(j);
        pthread_mutex_lock(&w->lock);
        w->pending--;
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}

void worker_start(Worker *w, const char *name, int nice) {
    memset(w, 0, sizeof *w);
    w->name = name;
    w->nice = nice;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    pthread_create(&w->th, NULL, worker_loop, w);
}

void worker_submit(Worker *w, void (*fn)(void *), void *arg) {
    Job *j = calloc(1, sizeof *j);
    j->fn = fn;
    j->arg = arg;
    pthread_mutex_lock(&w->lock);
    if (w->tail) w->tail->next = j;
    else w->head = j;
    w->tail = j;
    w->pending++;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
}

void worker_wait(Worker *w, double timeout) {
    double end = mono() + timeout;
    pthread_mutex_lock(&w->lock);
    while (w->pending > 0 && mono() < end) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&w->cond, &w->lock, &ts);
    }
    pthread_mutex_unlock(&w->lock);
}

/* ------------------------------------------------------------- helpers */
void release_frame(int cookie) { cam_release(cookie); }

static void mark(const char *step) {
    if (A.boot_marks_n < 16) {
        snprintf(A.boot_marks[A.boot_marks_n], sizeof A.boot_marks[0], "%s", step);
        A.boot_mark_t[A.boot_marks_n++] = uptime();
    }
}

void mark_dirty(void) { A.dirty_at = mono(); }

void save_state(void) {
    A.dirty_at = 0;
    if (state_equal(&A.st, &A.saved)) return;
    state_save(&A.st, A.state_file);
    A.saved = A.st;
}

/* The Pi has no clock battery. The time last seen is kept by Zero Ichi's own
 * files on the card (their "modified" date): the next start carries on from
 * there (clock_from_card). */
void remember_clock(void) {
    sd_hold();
    if (file_exists(A.state_file)) utime(A.state_file, NULL);
    sd_release();                                  /* (the log too) */
    log_flush();
}

static void clock_from_card(void) {
    static const char *const names[] = {"zeroichi-log.txt", "state.json", "boot-time.txt"};
    double best = 0;
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++) {
        char p[700];
        snprintf(p, sizeof p, "%s/%s", A.data_dir, names[i]);
        double t = file_mtime(p);
        if (t > best) best = t;
    }
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    if (best <= now.tv_sec + 1 || geteuid() != 0 || getenv("ZEROICHI_SIM")) return;
    struct timespec ts = {(time_t)best + 1, 0};
    clock_settime(CLOCK_REALTIME, &ts);
}

const char *pi_health(char *buf, size_t cap) {
    buf[0] = 0;
    char *t = read_file("/sys/class/thermal/thermal_zone0/temp", NULL);
    if (t) {
        snprintf(buf, cap, "%.0f C", atoi(t) / 1000.0);
        free(t);
    }
    char *f = read_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq", NULL);
    if (f) {
        size_t n = strlen(buf);
        snprintf(buf + n, cap - n, "%s%d MHz", n ? ", " : "", atoi(f) / 1000);
        free(f);
    }
    glob_t g;
    if (glob("/sys/devices/platform/soc*/*firmware*/get_throttled", 0, NULL, &g) == 0 && g.gl_pathc) {
        char *th = read_file(g.gl_pathv[0], NULL);
        if (th) {
            unsigned bits = (unsigned)strtoul(th, NULL, 16);
            static const char *const names[4] = {"under-voltage", "speed capped", "throttled", "temperature limit"};
            for (int b = 0; b < 4; b++)
                if (bits >> b & 1) {
                    size_t n = strlen(buf);
                    snprintf(buf + n, cap - n, ", %s now", names[b]);
                } else if (bits >> (b + 16) & 1) {
                    size_t n = strlen(buf);
                    snprintf(buf + n, cap - n, ", %s earlier", names[b]);
                }
            free(th);
        }
        globfree(&g);
    }
    if (!buf[0]) snprintf(buf, cap, "-");
    return buf;
}

/* where the processor's time went: this program, the camera server, the rest */
static double cpu_prev_self, cpu_prev_cam, cpu_prev_busy, cpu_prev_idle, cpu_prev_t;
static int camd_pid(void);

static double proc_cpu(const char *path) {
    char *t = read_file(path, NULL);
    if (!t) return 0;
    char *r = strrchr(t, ')');
    double v = 0;
    if (r) {
        long ut = 0, st = 0;
        if (sscanf(r + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st) == 2)
            v = (ut + st) / (double)sysconf(_SC_CLK_TCK);
    }
    free(t);
    return v;
}

void cpu_report(char *buf, size_t cap, int restart) {
    double self = proc_cpu("/proc/self/stat"), cam = 0;
    int pid = camd_pid();
    if (pid > 0) {
        char p[64];
        snprintf(p, sizeof p, "/proc/%d/stat", pid);
        cam = proc_cpu(p);
    }
    double busy = 0, idle = 0;
    char *t = read_file("/proc/stat", NULL);
    if (t) {
        long v[8] = {0};
        sscanf(t, "cpu %ld %ld %ld %ld %ld %ld %ld %ld", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
        double tick = sysconf(_SC_CLK_TCK);
        idle = (v[3] + v[4]) / tick;
        for (int i = 0; i < 8; i++) busy += v[i] / tick;
        busy -= idle;
        free(t);
    }
    double now = mono();
    double wall = fmax(1e-3, now - cpu_prev_t) * (sysconf(_SC_NPROCESSORS_ONLN) > 0 ? sysconf(_SC_NPROCESSORS_ONLN) : 1);
    double ds = self - cpu_prev_self, dc = cam - cpu_prev_cam, db = busy - cpu_prev_busy, di = idle - cpu_prev_idle;
    snprintf(buf, cap, "Zero Ichi %.0f%%, camera server %.0f%%, rest of the system %.0f%%, idle %.0f%%",
             100 * ds / wall, 100 * dc / wall, 100 * fmax(0, db - ds - dc) / wall, 100 * fmax(0, di) / wall);
    if (restart || cpu_prev_t == 0) {
        cpu_prev_self = self;
        cpu_prev_cam = cam;
        cpu_prev_busy = busy;
        cpu_prev_idle = idle;
        cpu_prev_t = now;
    }
}

static void set_governor(const char *g) {
    glob_t gl;
    if (glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor", 0, NULL, &gl)) return;
    for (size_t i = 0; i < gl.gl_pathc; i++) write_file(gl.gl_pathv[i], g, strlen(g), 0);
    globfree(&gl);
}

/* (v50) the sizes the hardware JPEG encoder was checked for: kept on the
 * system partition for the next start (see hwjpeg_remember) */
#define HW_CHECKS "/var/lib/zeroichi/hwjpeg.txt"
static void save_hw_checks(void *arg) {
    (void)arg;
    if (!hwjpeg_unsaved()) return;
    sys_write(1);
    if (hwjpeg_save()) logw("Hardware JPEG: could not note the checked sizes");
    sys_write(0);
}

/* ------------------------------------------------------------- the camera server */
static int camd_pid(void) { return cam_pid(); }

static int camd_attached;

static void spawn_camera(void) {
    const char *args[] = {"/usr/bin/python3", "-S", camd_path, NULL};
    const char *py = getenv("ZEROICHI_PYTHON");          /* (tests) */
    if (py) args[0] = py;
    cam_spawn_t = mono();
    /* the fast start started the camera server already (first thing, so the
     * camera library loads meanwhile): connect to it - once */
    static int tried;
    const char *pre = getenv("ZEROICHI_CAMD");
    if (!tried && pre && *pre) {
        tried = 1;
        if (cam_attach("zeroichi-camd", 3.0) == 0) {
            camd_attached = 1;
            return;
        }
    }
    camd_attached = 0;
    if (cam_spawn(args)) loge("Could not start the camera server");
}

/* ------------------------------------------------------------- screens */
static void switch_screen(const char *choice);
static void helper_files(void);

static int menu_choices(const char *key, char out[][16]) {
    int n = 0;
#define C(s) snprintf(out[n++], 16, "%s", s)
    if (!strcmp(key, "cam_rotate")) { C("0"); C("90"); C("180"); C("270"); }
    else if (!strcmp(key, "size")) { C("Screen"); C("4:3"); C("Square"); }
    else if (!strcmp(key, "clip_size")) { C(CLIP_NAMES[0]); C(CLIP_NAMES[1]); }
    else if (!strcmp(key, "gif_frames")) { C("8"); C("16"); C("24"); }
    else if (!strcmp(key, "video_max")) { C("15"); C("30"); C("60"); C("120"); C("300"); C("600"); C("720"); }
    else if (!strcmp(key, "screen")) { C("320x240"); C("240x240"); }
    else if (!strcmp(key, "screensaver")) { C("Off"); C("30 s"); C("1 min"); C("2 min"); C("5 min"); }
    else if (!strcmp(key, "key3")) { C("Power off"); C("Standby"); }
    else { C("0"); C("1"); }                       /* on / off */
#undef C
    return n;
}

static void current_value(const char *key, char *out) {
    State *s = &A.st;
    if (!strcmp(key, "cam_rotate")) sprintf(out, "%d", s->cam_rotate);
    else if (!strcmp(key, "size")) strcpy(out, s->size);
    else if (!strcmp(key, "clip_size")) strcpy(out, s->clip_size);
    else if (!strcmp(key, "gif_frames")) sprintf(out, "%d", s->gif_frames);
    else if (!strcmp(key, "video_max")) sprintf(out, "%d", s->video_max);
    else if (!strcmp(key, "screen")) sprintf(out, "%dx%d", A.W, A.H);
    else if (!strcmp(key, "screensaver")) strcpy(out, s->screensaver);
    else if (!strcmp(key, "key3")) strcpy(out, s->key3);
    else if (!strcmp(key, "mirror")) sprintf(out, "%d", s->mirror);
    else if (!strcmp(key, "screen_flip")) sprintf(out, "%d", s->screen_flip);
    else strcpy(out, "0");
}

static void set_value(const char *key, const char *v) {
    State *s = &A.st;
    if (!strcmp(key, "cam_rotate")) s->cam_rotate = atoi(v);
    else if (!strcmp(key, "size")) snprintf(s->size, sizeof s->size, "%s", v);
    else if (!strcmp(key, "clip_size")) snprintf(s->clip_size, sizeof s->clip_size, "%s", v);
    else if (!strcmp(key, "gif_frames")) s->gif_frames = atoi(v);
    else if (!strcmp(key, "video_max")) s->video_max = atoi(v);
    else if (!strcmp(key, "screensaver")) snprintf(s->screensaver, sizeof s->screensaver, "%s", v);
    else if (!strcmp(key, "key3")) snprintf(s->key3, sizeof s->key3, "%s", v);
    else if (!strcmp(key, "mirror")) s->mirror = atoi(v);
    else if (!strcmp(key, "screen_flip")) s->screen_flip = atoi(v);
}

static void apply_setting(const char *key) {
    if (!strcmp(key, "cam_rotate") || !strcmp(key, "mirror") || !strcmp(key, "size") ||
        !strcmp(key, "clip_size") || !strcmp(key, "mode"))
        apply_camera(0);
    if (!strcmp(key, "screen_flip")) {
        LcdConf c = *lcd_conf();
        c.flip = A.st.screen_flip;
        lcd_wait();
        lcd_open(&c, 0);
        flip_controls = A.st.screen_flip;
    }
}

/* (v53) MENU > About: a page of its own (about_draw) - any key: back */
static void about_show(void) {
    about_draw();
    lcd_show_now(A.scr.px);
}

static void about(void) {
    list_media();                                  /* (the count, as it is now) */
    free_space_update(1);
    A.screen = SCR_ABOUT;
    about_show();
}

static void on_about(int b) {
    (void)b;
    A.screen = SCR_MENU;
}

static void on_menu(int b) {
    if (A.presets_open) {
        presets_key(b);
        return;
    }
    int n = menu_count();
    const char *key = menu_key(A.menu_idx);
    int peeking = A.peek_until > mono();
    A.peek_until = 0;
    if (peeking && (b == BTN_UP || b == BTN_DOWN || b == BTN_PRESS)) return;   /* (just back to the MENU) */
    if (b == BTN_UP || b == BTN_DOWN) {
        A.menu_idx = (A.menu_idx + (b == BTN_DOWN ? 1 : -1) + n) % n;
    } else if (b == BTN_LEFT || b == BTN_RIGHT) {
        if (menu_is_action(A.menu_idx)) return;
        char ch[8][16], cur[32];
        int k = menu_choices(key, ch);
        current_value(key, cur);
        int i = 0;
        for (int j = 0; j < k; j++)
            if (!strcmp(ch[j], cur)) i = j;
        i = (i + (b == BTN_RIGHT ? 1 : -1) + k) % k;
        if (!strcmp(key, "screen")) {
            switch_screen(ch[i]);
        } else {
            set_value(key, ch[i]);
            apply_setting(key);
            mark_dirty();
            /* Rotation, Mirror, Image ratio: the picture itself for a moment, to see it */
            if (!strcmp(key, "cam_rotate") || !strcmp(key, "mirror") || !strcmp(key, "size"))
                A.peek_until = mono() + 3.0;
        }
    } else if (b == BTN_PRESS && menu_is_action(A.menu_idx)) {
        if (!strcmp(key, "presets")) {
            A.presets_open = 1;
            presets_refresh();
        } else if (!strcmp(key, "about")) about();
        else if (!strcmp(key, "erase")) erase_all_ask();
        else if (!strcmp(key, "photospace")) photo_space_action();
        else if (!strcmp(key, "poweroff")) {
            A.screen = SCR_CAMERA;
            power_off();
        }
    } else if (b == BTN_PRESS || b == BTN_KEY1 || b == BTN_KEY2) {
        A.screen = SCR_CAMERA;
        save_state();
    }
}

static void on_camera(int b) {
    State *s = &A.st;
    if (b == BTN_KEY1) {
        if (A.rec) stop_recording();
        else trigger();
    } else if (b == BTN_LEFT || b == BTN_RIGHT) {               /* works while recording too */
        double before = picture_aspect();
        s->filter = (s->filter + (b == BTN_RIGHT ? 1 : -1) + NLOOKS) % NLOOKS;
        if (picture_aspect() != before && !A.rec) apply_camera(0);    /* e.g. Holga is square */
        mark_dirty();
    } else if (A.rec) {
        return;
    } else if (b == BTN_UP || b == BTN_DOWN) {
        s->ev += b == BTN_UP ? 0.5 : -0.5;
        if (s->ev > 2) s->ev = 2;
        if (s->ev < -2) s->ev = -2;
        cam_set_ev(s->ev);
        mark_dirty();
    } else if (b == BTN_PRESS) {
        A.presets_open = 0;
        menu_refresh();
        A.screen = SCR_MENU;
    } else if (b == BTN_KEY2) {
        enter_gallery();
    } else if (b == BTN_KEY3) {
        const char *next = !strcmp(s->mode, "PHOTO") ? "GIF" : !strcmp(s->mode, "GIF") ? "VIDEO" : "PHOTO";
        snprintf(s->mode, sizeof s->mode, "%s", next);
        apply_camera(0);
        mark_dirty();
    }
}

/* ------------------------------------------------------------- developing screen */
void enter_developing(void) {
    A.screen = SCR_DEVELOPING;
    pthread_mutex_lock(&A.dev_lock);
    if (A.dev_preview.p) img_free(&A.dev_preview);
    pthread_mutex_unlock(&A.dev_lock);
    A.dev_shown = 0;
    cam_pause();
}

static void leave_developing(int done) {
    cam_resume();
    A.screen = SCR_CAMERA;
    if (!done) show_toast("Developing in background", 1.5);
}

void tick_developing(void) {
    if (!A.developer.pending) {
        pump_results();
        leave_developing(1);
        return;
    }
    double now = mono();
    if (now - A.dev_shown < 0.5) return;
    A.dev_shown = now;
    double p = A.dev_progress < 0 ? 0 : A.dev_progress;
    cv_fill(&A.scr, (Rgb){16, 16, 22});
    pthread_mutex_lock(&A.dev_lock);
    int have = A.dev_preview.p != NULL;
    if (have) {
        Img pv = A.dev_preview;
        double sc = fmin(A.W * 0.72 / pv.w, A.H * 0.52 / pv.h);
        if (sc > 1) sc = 1;
        int w = (int)(pv.w * sc), h = (int)(pv.h * sc);
        cv_blit_rgb(&A.scr, pv, (A.W - w) / 2, 40, w, h);
    }
    pthread_mutex_unlock(&A.dev_lock);
    cv_text(&A.scr, 18, A.W / 2, 20, "Developing film...", YELLOW, "mm", 1);
    int y = A.H - 58;
    cv_rect_outline(&A.scr, 20, y, A.W - 20, y + 14, (Rgb){200, 200, 210});
    cv_rect(&A.scr, 22, y + 2, 22 + (int)((A.W - 44) * p), y + 12, YELLOW);
    char pc[16];
    snprintf(pc, sizeof pc, "%d%%", (int)(p * 100));
    cv_text(&A.scr, 14, A.W / 2, have ? y - 10 : y - 12, pc, WHITE, "mm", 1);
    if (A.developer.pending > 1) {
        char t[48];
        snprintf(t, sizeof t, "%d videos to go", A.developer.pending);
        cv_text(&A.scr, 14, A.W / 2, y + 28, t, (Rgb){190, 190, 210}, "mm", 1);
    }
    cv_text(&A.scr, 14, A.W / 2, A.H - 12, "any key: back to the camera", (Rgb){150, 150, 170}, "mm", 1);
    draw_sd_dot();
    lcd_show(A.scr.px);
}

/* ------------------------------------------------------------- power */
static void standby(void) {
    if (A.rec) stop_recording();
    save_state();
    A.asleep = 1;
    lcd_sleep();
    cam_pause();
    set_governor("powersave");
    sync_all();
    logi("Standby (KEY3 held)");
}

static void wake_up(void) {
    if (A.asleep == 2) saver_free();
    A.asleep = 0;
    A.last_input = mono();
    set_governor("performance");
    lcd_wake();
    cam_resume();
    if (A.screen == SCR_CAMERA || A.screen == SCR_MENU) {     /* a fresh picture before the light */
        CamFrame f;
        if (cam_wait_frame(&f, 1.5)) tick_camera_frame(&f);
        lcd_wait();
    } else if (A.screen == SCR_GALLERY) {
        gallery_redraw();
    } else if (A.screen == SCR_DEVELOPING) {
        A.dev_shown = 0;
        tick_developing();
    } else if (A.screen == SCR_ABOUT) {
        about_show();
    }
    lcd_wait();
    lcd_backlight(1);
}

static double saver_timeout(void) {
    const char *s = A.st.screensaver;
    return !strcmp(s, "Off") ? 0 : !strcmp(s, "30 s") ? 30 : !strcmp(s, "2 min") ? 120 : !strcmp(s, "5 min") ? 300 : 60;
}

/* (v51) the screen saver is a picture (screensaver.c): Zero asleep, Bauhaus
 * shapes drifting by - the camera is paused meanwhile. After 10 minutes of it
 * the screen goes off (saves the battery). Any key: back to the camera. */
#define SAVER_DARK_AFTER 600.0

static void saver_tick(void) {
    double now = mono();
    if (now - A.saver_t0 >= SAVER_DARK_AFTER) {
        A.asleep = 1;
        saver_free();
        lcd_wait();
        lcd_sleep();
        logi("Screen saver: screen off (%.0f min of the screen saver)", SAVER_DARK_AFTER / 60);
        return;
    }
    if (now < A.saver_next) return;
    A.saver_next = fmax(A.saver_next + 1 / SAVER_FPS, now);       /* (no catching up) */
    saver_draw(&A.scr, now - A.saver_t0);
    draw_sd_dot();                                 /* (still saving: the red dot stays) */
    lcd_show(A.scr.px);
}

static void maybe_sleep(void) {
    double t = saver_timeout(), now = mono();
    /* (v51) never while Zero Ichi is busy - recording, saving a photo or GIF -
     * and the wait starts again when it's done: before, a 1-minute video
     * (with the 1-minute screen saver) ended in a dark screen, the saving
     * still going on behind it */
    if (A.rec || A.saver.pending) {
        A.last_input = now;
        return;
    }
    if (!t || A.asleep || now - A.last_input < t) return;
    A.asleep = 2;
    A.saver_t0 = A.saver_next = now;
    cam_pause();
    logi("Screen saver on (%s without a key press)", A.st.screensaver);
    saver_tick();
}

void app_exit(int code) {
    sd_end();                                      /* (the SD card writable again for what follows) */
    log_flush();
    sync_all();
    _exit(code);
}

void cleanup(int keep_screen) {
    static int done;
    if (done) return;
    done = 1;
    if (A.rec) stop_recording();
    if (A.dirty_at > 0) save_state();
    worker_wait(&A.saver, 60);
    cam_quit();
    buttons_stop();
    if (keep_screen) lcd_release();
    else lcd_close();
}

void power_off(void) {
    if (A.rec) stop_recording();
    save_state();
    wait_for_saves("Finishing saving...");
    if (A.S.allow_shutdown) {
        if (A.developer.pending) logi("Video developing will continue at the next start");
        show_message("Shutting down...", "unplug when the green LED is off", (Rgb){20, 20, 28});
        cleanup(1);
        remember_clock();
        sync_all();
        if (getenv("ZEROICHI_FASTINIT")) {
            const char *lines[] = {"Switched off", "", "Unplug when the", "green light is off."};
            brand_lines(lines, 4, ZI_INK);                       /* (v52: Zero Ichi's design) */
            save_message_screen();
            logi("Power off");
            app_exit(EXIT_POWEROFF);
        }
        const char *sudo = geteuid() == 0 ? "" : "sudo -n ";
        char cmd[128];
        snprintf(cmd, sizeof cmd, "%ssystemctl poweroff --force || %ssystemctl poweroff", sudo, sudo);
        run_cmd(cmd);
        for (;;) sleep(1);
    }
    show_message("Bye!", NULL, (Rgb){20, 20, 28});
    msleep(800);
    A.running = 0;
}

/* ------------------------------------------------------------- events */
void drain_events(void) {
    unsigned char m[64];
    while (read(btn_fd, m, sizeof m) > 0) {}
}

int key_pressed(int btn) {
    unsigned char m[2];
    int got = 0;
    while (read(btn_fd, m, 2) == 2)
        if (btn < 0 || m[0] == btn) got = 1;
    return got;
}

static void handle(int b, int kind) {
    static const int FLIP[NBTN] = {BTN_DOWN, BTN_UP, BTN_RIGHT, BTN_LEFT, BTN_PRESS, BTN_KEY1, BTN_KEY2, BTN_KEY3};
    if (flip_controls) b = FLIP[b];
    A.last_input = mono();
    if (A.splash_hold && !(b == BTN_KEY3 && kind == EV_HOLD)) return;   /* (starting: keys wait for the live view) */
    if (b == BTN_KEY3 && kind == EV_HOLD) {
        if (!strcmp(A.st.key3, "Standby")) {
            if (A.asleep) wake_up();
            else if (A.screen == SCR_CAMERA || A.screen == SCR_MENU || A.screen == SCR_GALLERY ||
                     A.screen == SCR_ABOUT) {
                A.screen = SCR_CAMERA;
                standby();
            }
            return;
        }
        power_off();
        return;
    }
    if (A.asleep) {                                /* a key wakes the screen - nothing more */
        wake_up();
        return;
    }
    switch (A.screen) {
    case SCR_CAMERA: on_camera(b); break;
    case SCR_MENU: on_menu(b); break;
    case SCR_GALLERY: on_gallery(b); break;
    case SCR_CONFIRM: on_confirm(b); break;
    case SCR_ERASE: on_erase(b); break;
    case SCR_DEVELOPING: leave_developing(0); break;
    case SCR_SPACE: on_space(b); break;
    case SCR_ABOUT: on_about(b); break;
    }
}

/* ------------------------------------------------------------- the screen */
static const char *screen_for(const char *choice) {
    if (!strcmp(choice, "240x240")) return "1.3inch-hat";
    const char *d = A.S.display;
    return !strcmp(d, "2.8inch-hat") || !strcmp(d, "st7789-320x240") || !strcmp(d, "ili9341-320x240") ? d : "2.8inch-hat";
}

static void lcd_conf_from_settings(const char *profile, LcdConf *c) {
    lcd_profile(profile, c);
    c->speed = A.S.spi_speed;
    c->bits = A.S.display_bits == 18 ? 18 : 16;
    if (A.S.display_madctl >= 0) c->madctl = A.S.display_madctl;
    if (A.S.display_invert >= 0) c->invert = A.S.display_invert;
    if (A.S.pin_dc >= 0) c->dc = A.S.pin_dc;
    if (A.S.pin_rst >= 0) c->rst = A.S.pin_rst;
    if (A.S.pin_bl >= 0) c->bl = A.S.pin_bl;
    if (A.S.spi_cs >= 0) c->cs = A.S.spi_cs;
    c->flip = A.st.screen_flip;
}

static void set_canvas(void) {
    A.W = lcd_conf()->w;
    A.H = lcd_conf()->h;
    cv_free(&A.scr);
    A.scr = cv_new(A.W, A.H);
}

static void switch_screen(const char *choice) {
    const char *profile = screen_for(choice);
    if (!strcmp(profile, lcd_conf()->profile)) return;
    LcdConf c;
    lcd_conf_from_settings(profile, &c);
    lcd_wait();
    lcd_open(&c, 1);
    set_canvas();
    snprintf(A.st.screen, sizeof A.st.screen, "%s", profile);
    state_set_screen_of(&A.st, pi_serial, profile);   /* (this Zero Ichi's screen, from now on) */
    helper_files();
    save_state();
    apply_camera(0);
    logi("Screen switched to %s (%dx%d)", profile, A.W, A.H);
}

/* settings loaded all at once (a preset): set up whatever changed */
void state_loaded(const State *old, const char *screen) {
    apply_camera(0);
    cam_set_ev(A.st.ev);
    if (old->screen_flip != A.st.screen_flip) apply_setting("screen_flip");
    if (screen && screen[0]) {
        char cur[16];
        snprintf(cur, sizeof cur, "%dx%d", A.W, A.H);
        if (strcmp(cur, screen)) switch_screen(screen);
    }
    mark_dirty();
}

/* the start-up picture for the fast start's zeroichi-lcd: kept up to date */
static void helper_files(void) {
    if (geteuid() != 0 || !file_exists("/var/lib/zeroichi")) return;
    char shared[128], own[128];
    helper_conf_path(shared, sizeof shared, 0);
    helper_conf_path(own, sizeof own, 1);
    int c1 = !lcd_helper_conf_same(shared), c2 = strcmp(own, shared) && !lcd_helper_conf_same(own);
    if (c1 || c2) {                                /* (only when the screen changed) */
        sys_write(1);
        if (c1) lcd_write_helper_conf(shared);
        if (c2) lcd_write_helper_conf(own);
        sys_write(0);
    }
}

static void show_splash(void) {
    char src[700];
    snprintf(src, sizeof src, "%s/%s", A.copy_dir, A.W == 240 ? "splash.rgb565" : "splash-320x240.rgb565");
    size_t n;
    uint8_t *d = (uint8_t *)read_file(src, &n);
    if (d && n == (size_t)A.W * A.H * 2) {
        for (int i = 0; i < A.W * A.H; i++) A.scr.px[i] = (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]);
        lcd_show_now(A.scr.px);
    } else {
        splash_screen();
    }
    free(d);
}

/* ------------------------------------------------------------- updates */
static int same_file(const char *a, const char *b) {
    struct stat sa, sb;
    if (stat(a, &sa) || stat(b, &sb) || sa.st_size != sb.st_size) return 0;
    if (labs((long)(sa.st_mtime - sb.st_mtime)) <= 2) return 1;
    size_t na, nb;
    char *x = read_file(a, &na), *y = read_file(b, &nb);
    int same = x && y && na == nb && !memcmp(x, y, na);
    free(x);
    free(y);
    return same;
}

/* files changed on the SD card (a new version, an edited settings.txt) */
static int sd_changed(char *list, size_t cap) {
    DIR *d = opendir(SD_DIR);
    if (!d) return 0;
    int n = 0;
    list[0] = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || !strncmp(nm, "toycam", 6)) continue;   /* (v53: the old name's files) */
        size_t l = strlen(nm);
        int ours = (l > 3 && !strcmp(nm + l - 3, ".py")) || (l > 7 && !strcmp(nm + l - 7, ".rgb565")) ||
                   !strcmp(nm, "settings.txt") || !strcmp(nm, "zeroichi-c");
        if (!ours) continue;
        char a[700], b[700];
        snprintf(a, sizeof a, "%s/%s", SD_DIR, nm);
        snprintf(b, sizeof b, "%s/%s", A.copy_dir, nm);
        if (!same_file(a, b)) {
            if (strlen(list) + l + 3 < cap) {
                strcat(list, list[0] ? ", " : "");
                strcat(list, nm);
            }
            n++;
        }
    }
    closedir(d);
    return n;
}

/* ------------------------------------------------------------- boot report */
/* What the kernel spent its first seconds on (its own log, /dev/kmsg): the
 * longest waits before Zero Ichi's fast start began, and - when the kernel is
 * started with "initcall_debug" (bootopt.py) - its slowest start-up steps */
typedef struct { double t, len; char msg[100], prev[100]; } KGap;
#define NTOP 8

static void top_add(KGap *top, double t, double len, const char *msg, const char *prev) {
    int i = NTOP - 1;
    if (len <= top[i].len) return;
    while (i > 0 && top[i - 1].len < len) {
        top[i] = top[i - 1];
        i--;
    }
    top[i].t = t;
    top[i].len = len;
    snprintf(top[i].msg, sizeof top[i].msg, "%s", msg);
    snprintf(top[i].prev, sizeof top[i].prev, "%s", prev ? prev : "");
}

static void buf_puts(Buf *b, const char *t) { buf_add(b, t, strlen(t)); }

static void kernel_report(Buf *b) {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    KGap gaps[NTOP], calls[NTOP], drivers[NTOP];
    memset(gaps, 0, sizeof gaps);
    memset(calls, 0, sizeof calls);
    memset(drivers, 0, sizeof drivers);
    char rec[1024], prev[100] = "";
    double prev_t = -1, init_t = 0;
    int ncalls = 0, ndrivers = 0;
    for (int tries = 0; tries < 100000; tries++) {
        ssize_t r = read(fd, rec, sizeof rec - 1);
        if (r < 0) {
            if (errno == EPIPE) continue;          /* (older lines were overwritten) */
            break;
        }
        rec[r] = 0;
        char *semi = strchr(rec, ';');
        int pr;
        unsigned long long seq, us;
        if (!semi || sscanf(rec, "%d,%llu,%llu", &pr, &seq, &us) != 3) continue;
        double t = us / 1e6;
        char msg[100];
        snprintf(msg, sizeof msg, "%.*s", (int)strcspn(semi + 1, "\n"), semi + 1);
        if (strstr(msg, "as init process")) init_t = t;
        if (init_t && !strncmp(msg, "initcall ", 9) && strchr(msg, '[')) {
            /* a driver loaded later (by the fast start): "initcall NAME+0x.. [module] returned .." */
            const char *af = strstr(msg, " after "), *br = strchr(msg, '[');
            long long n = af ? atoll(af + 7) : 0;
            char name[64];
            snprintf(name, sizeof name, "%.*s", (int)strcspn(br + 1, "]"), br + 1);
            top_add(drivers, t, n / 1e6, name, NULL);
            ndrivers++;
        }
        if (!init_t) {
            if (!strncmp(msg, "initcall ", 9)) {    /* "initcall NAME+0x../0x.. returned 0 after 1234 usecs" */
                const char *af = strstr(msg, " after ");
                long long n = af ? atoll(af + 7) : 0;
                char name[64];
                snprintf(name, sizeof name, "%.*s", (int)strcspn(msg + 9, "+ "), msg + 9);
                top_add(calls, t, n / 1e6, name, NULL);
                ncalls++;
            } else if (strncmp(msg, "calling ", 8)) {
                if (prev_t >= 0) top_add(gaps, t, t - prev_t, msg, prev);
                prev_t = t;
                snprintf(prev, sizeof prev, "%s", msg);
            }
        }
    }
    close(fd);
    if (init_t <= 0) return;
    char line[300];
    int m = snprintf(line, sizeof line, "\nKernel start-up (its own log): the fast start began at %.2f s.\n", init_t);
    buf_add(b, line, m);
    if (ncalls) {
        buf_add(b, "Its slowest steps:\n", 19);
        for (int i = 0; i < NTOP && calls[i].len > 0.01; i++) {
            m = snprintf(line, sizeof line, "  %5.2f s  %.2f s  %s\n", calls[i].t, calls[i].len, calls[i].msg);
            buf_add(b, line, m);
        }
        if (ndrivers) {
            buf_puts(b, "The slowest drivers the fast start loaded (time to start each):\n");
            for (int i = 0; i < NTOP && drivers[i].len > 0.005; i++) {
                m = snprintf(line, sizeof line, "  %5.2f s  %.2f s  %s\n", drivers[i].t, drivers[i].len,
                             drivers[i].msg);
                buf_add(b, line, m);
            }
        }
    } else {
        buf_puts(b, "(Its step timing was off for this start - it is switched on by an update.)\n");
    }
    buf_add(b, "The longest waits between its messages:\n", 41);
    for (int i = 0; i < NTOP && gaps[i].len > 0.05; i++) {
        m = snprintf(line, sizeof line, "  %5.2f s  +%.2f s before: %s\n    (after: %s)\n", gaps[i].t, gaps[i].len,
                     gaps[i].msg, gaps[i].prev);
        buf_add(b, line, m);
    }
}

/* the fast start's steps (fast-start.sh and the camera server note them:
 * "seconds-since-the-kernel-started step @idle-seconds-so-far"), in time
 * order, with how busy the processor was since the step before */
typedef struct { double t, idle; char text[120]; } FsStep;

static int fs_cmp(const void *a, const void *b) {
    const FsStep *x = a, *y = b;
    return x->t < y->t ? -1 : x->t > y->t;
}

static void fast_start_report(Buf *b) {
    char *fs = read_file("/run/zeroichi/fast-start.txt", NULL);
    if (!fs) return;
    FsStep st[64];
    int n = 0;
    for (char *ln = strtok(fs, "\n"); ln && n < 64; ln = strtok(NULL, "\n")) {
        char *sp = strchr(ln, ' ');
        if (!sp) continue;
        *sp = 0;
        FsStep *e = &st[n];
        e->t = atof(ln);
        e->idle = -1;
        char *at = strstr(sp + 1, " @");
        if (at) {
            e->idle = atof(at + 2);
            *at = 0;
        }
        snprintf(e->text, sizeof e->text, "%s", sp + 1);
        n++;
    }
    free(fs);
    qsort(st, (size_t)n, sizeof st[0], fs_cmp);   /* (several programs write it: by time) */
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpus < 1) cpus = 1;
    buf_puts(b, "Fast start (seconds after the kernel started; \"busy\": how much the processor\n"
                "worked since the line before - near 100% means everything was waiting for it):\n");
    double pt = -1, pidle = -1;
    for (int i = 0; i < n; i++) {
        char line[256], busy[24] = "";
        if (st[i].idle >= 0 && pidle >= 0 && st[i].t - pt >= 0.05) {
            double f = 1 - (st[i].idle - pidle) / ((st[i].t - pt) * (double)cpus);
            snprintf(busy, sizeof busy, "busy %3.0f%%", (f < 0 ? 0 : f > 1 ? 1 : f) * 100);
        }
        int m = snprintf(line, sizeof line, "  %6.2f s  %-46s %s\n", st[i].t, st[i].text, busy);
        buf_add(b, line, (size_t)m);
        if (st[i].idle >= 0) {
            pt = st[i].t;
            pidle = st[i].idle;
        }
    }
}

static void *boot_report(void *arg) {
    (void)arg;
    sleep(3);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    Buf b = {0};
    char line[512];
    int n = snprintf(line, sizeof line, "Zero Ichi v%s ready %.1f s after power-on (%s, C program)\n  [", VERSION,
                     A.ready_after, getenv("ZEROICHI_FASTINIT") ? "fast start" : "normal start-up");
    buf_add(&b, line, n);
    for (int i = 0; i < A.boot_marks_n; i++) {
        n = snprintf(line, sizeof line, "%s%s %.1fs", i ? "  " : "", A.boot_marks[i], A.boot_mark_t[i]);
        buf_add(&b, line, n);
    }
    n = snprintf(line, sizeof line, "]\n  camera server: %s\n\n", cam_ready_info);
    buf_add(&b, line, n);
    fast_start_report(&b);
    kernel_report(&b);
    size_t en;
    char *errs = read_file("/run/zeroichi/errors.txt", &en);
    if (errs && en) {
        buf_add(&b, "\nMessages:\n", 11);
        size_t from = en > 3000 ? en - 3000 : 0;
        buf_add(&b, errs + from, en - from);
    }
    free(errs);
    char p[700];
    snprintf(p, sizeof p, "%s/boot-time.txt", A.data_dir);
    write_file(p, b.p, b.n, 1);
    buf_free(&b);
    if (A.S.diagnostics) {                         /* the system report (diag.py) */
        char cmd[1600];
        snprintf(cmd, sizeof cmd, "ZEROICHI_DIR='%s' nice -n 19 /usr/bin/python3 -c \"import sys; sys.path.insert(0, '%s'); "
                 "import diag; diag.write_diagnostics()\" >/dev/null 2>&1", A.data_dir, A.copy_dir);
        sd_hold();                                 /* (this thread waits for it: the card stays writable) */
        run_cmd(cmd);
        sd_release();
    }
    sd_started();                                  /* (the card: read-only from now when idle) */
    return NULL;
}

/* ------------------------------------------------------------- main */
static void exe_dir(char *out, size_t cap) {
    char p[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", p, sizeof p - 1);
    if (n <= 0) {
        snprintf(out, cap, ".");
        return;
    }
    p[n] = 0;
    char *s = strrchr(p, '/');
    if (s) *s = 0;
    snprintf(out, cap, "%s", p);
}

/* Starting up (the fast start): the camera is what everyone waits for, so
 * Zero Ichi itself takes a back seat (its own start-up is done long before the
 * camera is) - and gets its normal priority back once the camera is ready. */
#define START_NICE 4
static int start_niced;

static void start_priority(int on) {
    if (on) {
        if (getenv("ZEROICHI_FASTINIT") && geteuid() == 0 &&
            setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), START_NICE) == 0)
            start_niced = 1;
        return;
    }
    if (!start_niced) return;
    start_niced = 0;
    DIR *d = opendir("/proc/self/task");           /* every thread still at the start-up priority */
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        id_t tid = (id_t)atoi(e->d_name);
        if (tid > 0 && getpriority(PRIO_PROCESS, tid) == START_NICE) setpriority(PRIO_PROCESS, tid, 0);
    }
    closedir(d);
}

static void on_signal(int s) {
    (void)s;
    A.running = 0;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    t_started = mono();
    mark("start");
    start_priority(1);
    read_pi_serial();
    if (getenv("ZEROICHI_TEST_PLAN")) {                 /* (tests: only work out Photo space) */
        test_space_plan();
        return 0;
    }
    {                                              /* threads: room enough (musl's default is small) */
        pthread_attr_t ta;
        pthread_attr_init(&ta);
        pthread_attr_setstacksize(&ta, 512 << 10);
        pthread_setattr_default_np(&ta);
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    memset(&A, 0, sizeof A);
    A.dev_progress = -1;
    A.free_b = -1;                                 /* (free space: not known yet) */
    A.splash_hold = 1;
    A.splash_since = mono();
    pthread_mutex_init(&A.res_lock, NULL);
    pthread_mutex_init(&A.dev_lock, NULL);
    /* where things are: running from the copy on the main partition (fast,
     * no waiting for the SD card), the SD card's zeroichi folder has the data */
    exe_dir(A.copy_dir, sizeof A.copy_dir);
    const char *dd = getenv("ZEROICHI_DIR");
    int from_copy = !dd && !strcmp(A.copy_dir, "/var/lib/zeroichi/app");
    snprintf(A.data_dir, sizeof A.data_dir, "%s", dd ? dd : from_copy ? SD_DIR : A.copy_dir);
    if (dd) SD_DIR = dd;
    snprintf(A.state_file, sizeof A.state_file, "%s/state.json", A.data_dir);
    snprintf(camd_path, sizeof camd_path, "%s/camd.py", A.copy_dir);
    for (const char *wd = "/run/zeroichi"; wd; wd = NULL)
        if (mkdir_p(wd) == 0 && chdir(wd) == 0) break;
    /* settings (from the copy, kept in step with the SD card) */
    char sp[700];
    snprintf(sp, sizeof sp, "%s/settings.txt", A.copy_dir);
    if (!file_exists(sp)) snprintf(sp, sizeof sp, "%s/settings.txt", A.data_dir);
    settings_read(sp, &A.S);
    if (!strcmp(A.S.app, "python") || A.S.no_camera) return EXIT_USE_PYTHON;   /* (no_camera: a test mode of the Python version) */
    cam_swap_uv(A.S.swap_rb);
    snprintf(A.photo_setting, sizeof A.photo_setting, "%s", A.S.photo_dir);
    /* the camera first: it takes the longest (its server loads meanwhile) */
    looks_init();
    if (geteuid() == 0 && !getenv("ZEROICHI_SIM") && file_exists("/var/lib/zeroichi")) hwjpeg_remember(HW_CHECKS);
    spawn_camera();
    mark(camd_attached ? "camera server connected" : "camera server started");
    /* the SD card's ZEROICHI drive: everything set in the camera is kept there
     * (and only there). Running from the copy, it may still be being mounted
     * - the camera server needs longer to start anyway. */
    if (from_copy) {
        double end = mono() + 90;
        while (mono() < end && !(is_mount("/boot/firmware") && file_exists(SD_DIR))) {
            cam_read();
            msleep(20);
        }
        mark("sd card");
    }
    clock_from_card();
    state_default(&A.st, &A.S);
    state_load(&A.st, A.state_file);
    /* the screen: this Zero Ichi's own (the card remembers each one's), else the
     * one used last, else settings.txt's */
    const char *mine = state_screen_of(&A.st, pi_serial);
    if (mine) snprintf(A.st.screen, sizeof A.st.screen, "%s", mine);
    const char *profile = A.st.screen[0] ? A.st.screen : A.S.display;
    int key2 = A.S.pin_key[BTN_KEY2] >= 0 ? A.S.pin_key[BTN_KEY2] : 20;
    if (button_held_now(key2)) {                   /* KEY2 held: the other screen type */
        const char *p320 = strcmp(A.S.display, "1.3inch-hat") ? A.S.display : "2.8inch-hat";
        profile = strcmp(profile, "1.3inch-hat") ? "1.3inch-hat" : p320;
        snprintf(A.st.screen, sizeof A.st.screen, "%s", profile);
        state_set_screen_of(&A.st, pi_serial, profile);
        state_save(&A.st, A.state_file);
        logi("KEY2 held at start-up: screen switched to %s", profile);
    }
    LcdConf lc;
    lcd_conf_from_settings(profile, &lc);
    /* the fast start's zeroichi-lcd has shown the picture already: set the
     * screen up again without the reset (which would blank it) */
    int shown = file_exists("/run/zeroichi/splash-ok");
    char cpath[128];
    helper_conf_path(cpath, sizeof cpath, 1);         /* (the one zeroichi-lcd used) */
    if (!file_exists(cpath)) helper_conf_path(cpath, sizeof cpath, 0);
    char *old = read_file(cpath, NULL);
    int same = 0;
    if (old) {
        LcdConf tmp = lc;
        (void)tmp;
        char want[512];
        snprintf(want, sizeof want, "controller %s\nwidth %d\nheight %d\nmadctl %d\ninvert %d\ndc %d\nrst %d\nbl %d\ncs %d\nspeed %d\nflip %d\n",
                 lc.controller, lc.w, lc.h, lc.madctl, lc.invert ? 1 : 0, lc.dc, lc.rst, lc.bl, lc.cs, lc.speed, lc.flip ? 1 : 0);
        same = !strcmp(old, want);
        free(old);
    }
    if (lcd_open(&lc, !(shown && same))) {
        loge("Screen could not be opened");
    }
    set_canvas();
    /* (shown straight through the SPI hardware: drawn once more the usual way,
     * in case that went wrong somewhere) */
    if (!(shown && same) || file_exists("/run/zeroichi/splash-direct")) show_splash();
    lcd_backlight(1);
    mark("screen");
    helper_files();
    flip_controls = A.st.screen_flip;
    /* buttons */
    int pins[NBTN] = {6, 19, 5, 26, 13, 21, 20, 16};
    for (int i = 0; i < NBTN; i++)
        if (A.S.pin_key[i] >= 0) pins[i] = A.S.pin_key[i];
    const char *inj = getenv("ZEROICHI_SIM_BUTTONS");
    if (inj) {
        int fd = open(inj, O_RDONLY);
        if (fd >= 0) buttons_inject_fd(fd);
    }
    btn_fd = buttons_start(pins, BTN_KEY3, HOLD_SECONDS);
    /* camera set-up (sent now; the server applies it as soon as it has the camera) */
    apply_camera(0);
    char lp[700];
    snprintf(lp, sizeof lp, "%s/zeroichi-log.txt", A.data_dir);
    int cont = file_exists("/run/zeroichi/log-continues");      /* (restarted after an update) */
    unlink("/run/zeroichi/log-continues");
    /* the SD card read-only whenever nothing is being saved: pulling the plug
     * can't harm it then (the fast start - see sd_init) */
    if (from_copy || getenv("ZEROICHI_SD_TEST")) sd_init(A.data_dir, NULL);
    log_open(lp, cont);
    logi("Zero Ichi v%s starting %.1f s after power-on (C program) - display: %s%s", VERSION, uptime(),
         lcd_conf()->profile, from_copy ? " - fast start from copy" : "");
    char changed[512];
    if (from_copy && sd_changed(changed, sizeof changed)) {
        logi("Files changed on the SD card (%s) - updating", changed);
        show_message("Updating Zero Ichi...", "one moment", (Rgb){20, 20, 28});
        write_file("/run/zeroichi/log-continues", "", 0, 0);
        lcd_release();
        cam_abandon();
        app_exit(EXIT_UPDATE);
    }
    A.saved = A.st;
    choose_photo_dir();
    if (from_copy || getenv("ZEROICHI_SD_TEST")) sd_init(A.photo_dir, NULL);     /* (its drive too, e.g. PHOTOS) */
    if (sd_active()) {
        char dr[600];
        sd_drives(dr, sizeof dr);
        logi("SD card: read-only between saves (%s) - safe to unplug while there's no red dot", dr);
    }
    if (from_copy || getenv("ZEROICHI_SIM")) maybe_make_image(SD_DIR);
    list_media();
    A.count = A.nmedia;
    worker_start(&A.saver, "saver", 2);
    worker_start(&A.developer, "developer", 10);
    resume_developing();
    A.running = 1;
    A.screen = SCR_CAMERA;
    A.last_input = mono();
    A.lv_t0 = mono();
    A.clock_at = mono();
    cam_set_ev(A.st.ev);
    int ready_logged = 0, offered = 0, odd_fails = 0;
    cpu_report(changed, sizeof changed, 1);
    while (A.running) {
        struct pollfd fds[2] = {{btn_fd, POLLIN, 0}, {cam_socket(), POLLIN, 0}};
        int timeout = A.asleep == 2 ? (int)fmax(1, fmin(100, (A.saver_next - mono()) * 1000 + 1))
                      : A.asleep ? 100 : A.screen == SCR_CAMERA || A.screen == SCR_MENU ? 40 : 30;
        poll(fds, cam_socket() >= 0 ? 2 : 1, timeout);
        unsigned char m[2];
        while (read(btn_fd, m, 2) == 2)
            if (m[0] < NBTN) handle(m[0], m[1]);
        if (!A.running) break;
        cam_read();
        pump_results();
        /* the camera: not there (yet)? */
        A.cam_missing = !cam_configured();
        int no_cam = strstr(cam_error(), "no camera found") != NULL;
        if (cam_failed() && mono() - cam_spawn_t > (no_cam ? 5 : 1)) {
            /* the camera server itself went wrong (not a missing camera) before
             * the camera ever worked: the Python Zero Ichi takes over for now */
            if (!ready_logged && !no_cam && ++odd_fails >= 2) {
                loge("The camera server failed twice (%s) - starting the Python Zero Ichi instead", cam_error());
                show_message("One moment...", "starting the camera another way", (Rgb){20, 20, 28});
                write_file("/run/zeroichi/log-continues", "", 0, 0);
                cleanup(1);
                app_exit(EXIT_USE_PYTHON);
            }
            if (!camd_spawned_at_fail++) loge("No camera: %s - still trying", cam_error());
            cam_quit();
            spawn_camera();
            memset(A.cfg_key, 0, sizeof A.cfg_key);
            apply_camera(1);
        } else if (cam_ready() && !cam_configured() && mono() - cam_spawn_t > 1 && A.cfg_key[0] == 0) {
            apply_camera(1);
        }
        if (cam_configured() && !ready_logged) {
            CamFrame f;
            if (cam_read()) {
                ready_logged = 1;
                A.ready_after = uptime();
                mark("camera ready");
                char steps[512] = "";
                for (int i = 0; i < A.boot_marks_n; i++) {
                    size_t l = strlen(steps);
                    snprintf(steps + l, sizeof steps - l, "%s%s %.1fs", i ? "  " : "", A.boot_marks[i],
                             A.boot_mark_t[i]);
                }
                logi("Camera ready (%s)", cam_ready_info);
                if (pi_serial[0] && !state_screen_of(&A.st, pi_serial)) {   /* (this Zero Ichi's screen) */
                    state_set_screen_of(&A.st, pi_serial, lcd_conf()->profile);
                    mark_dirty();
                }
                logi("Ready %.1f s after power-on  [%s]", A.ready_after, steps);
                write_file("/run/zeroichi/camera-ready", "", 0, 0);   /* (v53: the fast start's other drivers now) */
                start_priority(0);
                pthread_t th;
                pthread_create(&th, NULL, boot_report, NULL);
                pthread_detach(th);
                (void)f;
            }
        }
        if (ready_logged && !offered) {
            offered = 1;
            maybe_offer_space();
        }
        if (A.asleep) {
            CamFrame f;
            while (cam_take(&f)) cam_release(f.cookie);
            if (A.asleep == 2) saver_tick();
            continue;
        }
        switch (A.screen) {
        case SCR_CAMERA:
        case SCR_MENU: camera_step(); break;
        case SCR_GALLERY: {
            CamFrame f;
            while (cam_take(&f)) cam_release(f.cookie);
            tick_gallery();
            break;
        }
        case SCR_DEVELOPING: tick_developing(); break;
        case SCR_SPACE: {
            CamFrame f;
            while (cam_take(&f)) cam_release(f.cookie);
            tick_space();
            break;
        }
        case SCR_ERASE:
        case SCR_ABOUT: {                          /* (the page stays as it is) */
            CamFrame f;
            while (cam_take(&f)) cam_release(f.cookie);
            break;
        }
        default: break;
        }
        if (A.screen != SCR_CAMERA && A.screen != SCR_MENU) live_rate(0, 0);   /* (back to 15 a second) */
        if (A.dirty_at > 0 && !A.rec && mono() - A.dirty_at > 0.5) save_state();
        if (hwjpeg_unsaved() && !A.rec && !A.saver.pending && !A.developer.pending)
            worker_submit(&A.developer, save_hw_checks, NULL);  /* (once there is news, when idle) */
        if (mono() - A.clock_at > (sd_active() ? 1800 : 300) && !A.rec) {   /* (read-only card: seldom) */
            A.clock_at = mono();
            remember_clock();
        }
        maybe_sleep();
    }
    cleanup(0);
    app_exit(0);                                   /* (no waiting for the other threads) */
    return 0;
}
