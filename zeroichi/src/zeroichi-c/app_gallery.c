/* Zero Ichi - the gallery: photos, GIFs (playing) and videos (press to play) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "app.h"
#include "gif.h"
#include "gpio.h"
#include "lcd.h"
#include "screensaver.h"                 /* (Zero Ichi's colours) */
#include "util.h"

typedef struct {
    char path[600], name[64];
    int kind;                       /* 'p' photo, 'g' GIF, 'v' video */
    int error;
    /* the picture(s) as screen frames */
    uint16_t **frames;
    double *durs;
    int n, i, done;
    double next;
    GifReader *gif;
    MovReader *mov;
} Gal;

#define G ((Gal *)A.gal)

static void free_gal(void) {
    Gal *g = G;
    if (!g) return;
    for (int i = 0; i < g->n; i++) free(g->frames[i]);
    free(g->frames);
    free(g->durs);
    if (g->gif) gifr_close(g->gif);
    if (g->mov) {
        movr_close(g->mov);
        free(g->mov);
    }
    free(g);
    A.gal = NULL;
}

/* an R,G,B picture fitted onto the screen (ImageOps.pad style: smooth) */
static uint16_t *to_screen(Img rgb, int smooth) {
    double sc = fmin((double)A.W / rgb.w, (double)A.H / rgb.h);
    int nw = pyround(rgb.w * sc), nh = pyround(rgb.h * sc);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    Img r = (nw == rgb.w && nh == rgb.h) ? rgb : img_resize(rgb, nw, nh, smooth ? RS_BILINEAR : RS_NEAREST);
    uint16_t *px = calloc((size_t)A.W * A.H, 2);
    int x0 = (A.W - nw) / 2, y0 = (A.H - nh) / 2;
    for (int y = 0; y < nh; y++) {
        const uint8_t *p = PX(r, 0, y);
        uint16_t *o = px + (size_t)(y0 + y) * A.W + x0;
        for (int x = 0; x < nw; x++, p += r.c)
            o[x] = r.c == 3 ? C565(p[0], p[1], p[2]) : C565(p[0], p[0], p[0]);
    }
    if (r.p != rgb.p) img_free(&r);
    return px;
}

static void add_frame(Gal *g, uint16_t *px, double dur) {
    g->frames = realloc(g->frames, sizeof(uint16_t *) * (g->n + 1));
    g->durs = realloc(g->durs, sizeof(double) * (g->n + 1));
    g->frames[g->n] = px;
    g->durs[g->n] = dur;
    g->n++;
}

/* a video frame decoded at about the screen size (the JPEG decoder does
 * 1/2, 1/4, 1/8 almost for free), turned upright */
static Img video_frame(MovReader *rd, int i) {
    size_t len;
    uint8_t *d = movr_frame(rd, i, &len);
    Img none = {0};
    if (!d) return none;
    int w = rd->w, h = rd->h, turned = rd->rotation == 90 || rd->rotation == 270;
    double sc = fmin((double)A.W / (turned ? h : w), (double)A.H / (turned ? w : h)) * 0.35;
    int dw = (int)(w * sc), dh = (int)(h * sc);
    Img a = jpeg_decode(d, len, dw, dh, 0);
    free(d);
    if (!a.p) return a;
    Img r = a;
    if (rd->rotation == 90) r = img_rot90cw(a);
    else if (rd->rotation == 270) r = img_rot90ccw(a);
    else if (rd->rotation == 180) r = img_rot180(a);
    if (r.p != a.p) img_free(&a);
    return r;
}

static int gif_decode_next(Gal *g) {
    if (!g->gif || g->done) return 0;
    Img rgb;
    double s;
    if (!gifr_next(g->gif, &rgb, &s)) {
        g->done = 1;
        gifr_close(g->gif);
        g->gif = NULL;
        return 0;
    }
    add_frame(g, to_screen(rgb, 0), s);
    if (g->n >= 60) {
        g->done = 1;
        gifr_close(g->gif);
        g->gif = NULL;
    }
    return 1;
}

static int drawn_dot;                     /* (the red dot as last drawn - see draw_sd_dot) */

static void draw_item(void) {
    Gal *g = G;
    if (!g) return;
    memcpy(A.scr.px, g->frames[g->i], (size_t)A.W * A.H * 2);
    /* (v53) its name only - no date (the Pi has no clock of its own) */
    char stamp[96];
    snprintf(stamp, sizeof stamp, "%s", g->name);
    char *dot = strrchr(stamp, '.');
    if (dot) *dot = 0;
    cv_text(&A.scr, 14, A.W / 2, 14, stamp, WHITE, "mm", 1);
    char tag[32] = "";
    if (g->kind == 'g') snprintf(tag, sizeof tag, "GIF");
    if (g->kind == 'v') {
        int s = g->mov ? (int)lround(movr_duration(g->mov)) : 0;
        snprintf(tag, sizeof tag, "VIDEO %d:%02d", s / 60, s % 60);
    }
    if (tag[0])
        cv_text(&A.scr, 14, 6, 36, tag, g->kind == 'g' ? (Rgb){120, 220, 255} : (Rgb){255, 110, 110}, "lm", 1);
    if (g->error) {
        cv_text(&A.scr, 18, A.W / 2, A.H / 2, "Can't open file", WHITE, "mm", 1);
    } else if (g->kind == 'v') {
        cv_text(&A.scr, 30, A.W / 2, A.H / 2 - 8, "▶", WHITE, "mm", 1);
        cv_text(&A.scr, 14, A.W / 2, A.H / 2 + 22, "PRESS TO PLAY", WHITE, "mm", 1);
    }
    char pos[48];
    snprintf(pos, sizeof pos, "<  %d/%d  >", A.gidx + 1, A.nmedia);
    cv_text(&A.scr, 18, A.W / 2, A.H - 14, pos, WHITE, "mm", 1);
    drawn_dot = sd_writable();
    draw_sd_dot();
    lcd_show(A.scr.px);
}

static void load_item(void) {
    free_gal();
    if (!A.nmedia) {
        show_message("No photos yet", "KEY2: back to camera", (Rgb){20, 20, 28});
        return;
    }
    Gal *g = calloc(1, sizeof *g);
    A.gal = g;
    snprintf(g->name, sizeof g->name, "%s", A.media[A.gidx]);
    snprintf(g->path, sizeof g->path, "%s/%s", A.photo_dir, g->name);
    g->kind = !strncmp(g->name, "IMG_", 4) ? 'p' : !strncmp(g->name, "GIF_", 4) ? 'g' : 'v';
    if (g->kind == 'p') {
        size_t n;
        uint8_t *d = (uint8_t *)read_file(g->path, &n);
        Img a = d ? jpeg_decode(d, n, A.W, A.H, 0) : (Img){0};
        free(d);
        if (a.p) {
            add_frame(g, to_screen(a, 1), 0);
            img_free(&a);
        } else {
            g->error = 1;
        }
    } else if (g->kind == 'g') {
        g->gif = gifr_open(g->path);
        if (!g->gif || !gif_decode_next(g)) g->error = 1;
    } else {
        g->mov = calloc(1, sizeof(MovReader));
        if (movr_open(g->mov, g->path) == 0) {
            Img a = video_frame(g->mov, 0);
            if (a.p) {
                add_frame(g, to_screen(a, 0), 0);
                img_free(&a);
            } else {
                g->error = 1;
            }
        } else {
            free(g->mov);
            g->mov = NULL;
            g->error = 1;
        }
    }
    if (g->error) {
        logw("Can't open %s", g->name);
        uint16_t *px = malloc((size_t)A.W * A.H * 2);
        for (int i = 0; i < A.W * A.H; i++) px[i] = C565(60, 0, 0);
        if (g->n) {
            for (int i = 0; i < g->n; i++) free(g->frames[i]);
            g->n = 0;
        }
        add_frame(g, px, 0);
        if (g->gif) {
            gifr_close(g->gif);
            g->gif = NULL;
        }
        g->done = 1;
    }
    draw_item();
}

void gallery_redraw(void) {
    if (G) draw_item();
    else if (A.screen == SCR_GALLERY) load_item();
}

void enter_gallery(void) {
    wait_for_saves("Saving...");
    list_media();
    A.gidx = A.nmedia - 1;
    A.screen = SCR_GALLERY;
    load_item();
}

void tick_gallery(void) {
    Gal *g = G;
    if (g && A.screen == SCR_GALLERY && sd_writable() != drawn_dot) draw_item();   /* (the red dot came / went) */
    if (!g || g->kind != 'g' || g->error) return;
    double now = mono();
    if (now < g->next) return;
    int nxt = g->i + 1;
    if (nxt >= g->n) {
        if (!(!g->done && gif_decode_next(g))) nxt = 0;
    }
    if (nxt < g->n && g->n > 1) {
        g->i = nxt;
        g->next = now + g->durs[g->i];
        draw_item();
    }
}

static void play_video(void) {
    Gal *g = G;
    MovReader *rd = g->mov;
    drain_events();
    int shown = 0;
    double t0 = mono(), due = 0;
    for (int i = 0; i < rd->n; i++) {
        if (key_pressed(-1)) break;                  /* any key stops playback */
        double due_next = due + rd->durations[i];
        if (mono() - t0 > due_next) {                /* already too late for this one */
            due = due_next;
            continue;
        }
        Img a = video_frame(rd, i);
        if (a.p) {
            uint16_t *px = to_screen(a, 0);
            img_free(&a);
            double wait = t0 + due - mono();
            if (wait > 0) sleep_s(wait);
            memcpy(A.scr.px, px, (size_t)A.W * A.H * 2);
            free(px);
            lcd_show(A.scr.px);
            shown++;
        }
        due = due_next;
    }
    lcd_wait();
    double took = mono() - t0;
    A.last_input = mono();
    logi("Played %s: showed %d of %d frames (%.1f frames/s)", g->name, shown, rd->n, shown / fmax(took, 0.1));
    drain_events();
    draw_item();
}

void on_gallery(int b) {
    if (b == BTN_KEY1 || b == BTN_KEY2) {
        free_gal();
        A.screen = SCR_CAMERA;
        return;
    }
    if (!A.nmedia) return;
    if (b == BTN_LEFT || b == BTN_RIGHT) {
        A.gidx = (A.gidx + (b == BTN_RIGHT ? 1 : -1) + A.nmedia) % A.nmedia;
        load_item();
    } else if (b == BTN_UP) {
        A.gidx = 0;
        load_item();
    } else if (b == BTN_DOWN) {
        A.gidx = A.nmedia - 1;
        load_item();
    } else if (b == BTN_PRESS) {
        if (G && G->kind == 'v' && G->mov && !G->error) play_video();
    } else if (b == BTN_KEY3 && G) {
        A.screen = SCR_CONFIRM;
        char t[48];
        snprintf(t, sizeof t, "Delete %s?", G->kind == 'p' ? "photo" : G->kind == 'g' ? "GIF" : "video");
        show_message(t, "KEY3 = yes   other = no", (Rgb){90, 10, 10});
    }
}

void on_confirm(int b) {
    if (b == BTN_KEY3 && G) {
        if (sd_unlink(G->path) == 0) {
            sync_all();
            free(A.media[A.gidx]);
            memmove(&A.media[A.gidx], &A.media[A.gidx + 1], sizeof(char *) * (A.nmedia - A.gidx - 1));
            A.nmedia--;
            A.count = A.count > 0 ? A.count - 1 : 0;
            if (A.gidx >= A.nmedia) A.gidx = A.nmedia - 1;
            if (A.gidx < 0) A.gidx = 0;
            show_message("Deleted", NULL, (Rgb){20, 20, 28});
            msleep(500);
        } else {
            show_message("Delete failed", NULL, (Rgb){90, 10, 10});
            msleep(1500);
        }
    }
    A.screen = SCR_GALLERY;
    load_item();
}

/* ------------------------------------------------------------- erase all */
/* (0.1.0) MENU > Erase all: every photo, GIF and video on the card - asked
 * twice (KEY3, then KEY3 again; any other key: no). Only Zero Ichi's own
 * files (IMG_, GIF_, VID_ .jpg / .gif / .mov in the photos folder): the
 * settings, presets and Zero Ichi itself stay. (Not a "format": the card's
 * drive also holds Zero Ichi and your settings.) The numbers carry on
 * counting up, so new files never get an old file's name. */
static int erase_step;

static void erase_question(void) {
    char t[48], n[48];
    snprintf(t, sizeof t, "%s", erase_step == 1 ? "Erase all files?" : "Are you sure?");
    if (A.nmedia == 1) snprintf(n, sizeof n, "1 file");
    else snprintf(n, sizeof n, "%d photos, GIFs and videos", A.nmedia);
    const char *l1[] = {t, "", n, "KEY3 = yes   other = no"};
    const char *l2[] = {t, "", "They can't be brought back.", "KEY3 = erase   other = no"};
    brand_lines(erase_step == 1 ? l1 : l2, 4, ZI_RED);
    lcd_show_now(A.scr.px);
}

void erase_all_ask(void) {
    if (A.developer.pending) {
        show_toast("Developing a video -\ntry again in a moment", 2.5);
        return;
    }
    wait_for_saves("Saving...");
    list_media();
    A.count = A.nmedia;
    if (!A.nmedia) {
        show_toast("Nothing to erase", 1.5);
        return;
    }
    erase_step = 1;
    A.screen = SCR_ERASE;
    erase_question();
}

static void erase_now(void) {
    int n = A.nmedia, done = 0;
    double t0 = mono(), shown = 0;
    sd_hold();                                       /* (the card writable meanwhile) */
    for (int i = 0; i < n; i++) {
        char p[768];
        snprintf(p, sizeof p, "%s/%s", A.photo_dir, A.media[i]);
        if (unlink(p) == 0) done++;
        if (mono() - shown > 0.3) {                  /* (a big card can take a while) */
            char s[48];
            snprintf(s, sizeof s, "%d of %d", i + 1, n);
            show_message("Erasing...", s, (Rgb){20, 20, 28});
            shown = mono();
        }
    }
    sync_all();
    sd_release();
    list_media();
    A.count = A.nmedia;
    A.gidx = 0;
    free_space_update(1);
    logi("Erase all: %d of %d photos, GIFs and videos erased in %.1f s", done, n, mono() - t0);
    if (done == n) {
        show_message("Erased", n == 1 ? "1 file" : "all photos, GIFs and videos", (Rgb){20, 20, 28});
    } else {
        char s[48];
        snprintf(s, sizeof s, "%d could not be erased", n - done);
        show_message("Erase failed", s, (Rgb){90, 10, 10});
    }
    msleep(1500);
    drain_events();
}

void on_erase(int b) {
    if (b == BTN_KEY3 && erase_step == 1) {
        erase_step = 2;
        erase_question();
        return;
    }
    if (b == BTN_KEY3 && erase_step == 2) erase_now();
    erase_step = 0;
    A.screen = SCR_MENU;
}
