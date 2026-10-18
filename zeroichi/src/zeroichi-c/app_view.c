/* Zero Ichi - picture sizes, the camera set-up, the live view and everything
 * drawn over it (a port of app.py's drawing and camera.py's size rules). */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "font.h"
#include "screensaver.h"
#include "brandart.h"
#include "lcd.h"
#include "util.h"

/* (v55) two clip sizes (CLIP_NAMES): width, height - and square looks' */
static const int CLIP[2][4] = {{320, 240, 240, 240}, {640, 480, 480, 480}};
static const Rgb MODE_COLORS[3] = {{255, 255, 255}, {120, 220, 255}, {255, 110, 110}};
static const Rgb REC_RED = {255, 60, 60};

int mode(void) {
    return !strcmp(A.st.mode, "GIF") ? MODE_GIF : !strcmp(A.st.mode, "VIDEO") ? MODE_VIDEO : MODE_PHOTO;
}
const char *mode_name(void) { return A.st.mode; }

static int clip_index(void) {
    return strcmp(A.st.clip_size, CLIP_NAMES[0]) ? 1 : 0;
}

/* ------------------------------------------------------------- geometry */
double picture_aspect(void) {
    if (look_square(A.st.filter) || !strcmp(A.st.size, "Square")) return 1.0;
    if (!strcmp(A.st.size, "4:3")) return 4.0 / 3.0;
    return (double)A.W / A.H;
}

void flips_for(int rot, int mirror, int *h, int *v, int *turn) {
    int k = (rot / 90) % 4;
    *h = *v = k >= 2;
    if (mirror) {
        if (k % 2 == 0) *h = !*h;
        else *v = !*v;
    }
    *turn = k % 2 == 1;
}

void target_size(const char *kind, int *w, int *h) {
    double a = picture_aspect();
    int square = a == 1.0;
    if (!strcmp(kind, "photo")) {
        int hf, vf, turn;
        flips_for(A.st.cam_rotate, A.st.mirror, &hf, &vf, &turn);
        int aw = turn ? PHOTO_H : PHOTO_W, ah = turn ? PHOTO_W : PHOTO_H;
        int pw, ph;
        if ((double)aw / ah >= a) {
            ph = ah;
            pw = pyround(ah * a);
        } else {
            pw = aw;
            ph = pyround(aw / a);
        }
        *w = pw - pw % 2;
        *h = ph - ph % 2;
        return;
    }
    if (!strcmp(kind, "clip")) {
        int i = clip_index();
        *w = CLIP[i][square ? 2 : 0];
        *h = CLIP[i][square ? 3 : 1];
        return;
    }
    int vw, vh;                                   /* the live view: fitted to the screen */
    if (a >= (double)A.W / A.H) {
        vw = A.W;
        vh = pyround(A.W / a);
    } else {
        vw = pyround(A.H * a);
        vh = A.H;
    }
    *w = vw & ~1;
    *h = vh & ~1;
}

static void sensor_size_for(int tw, int th, int turn, int *sw, int *sh) {
    int h = turn ? (tw > (th * 3 + 3) / 4 ? tw : (th * 3 + 3) / 4) : (th > (tw * 3 + 3) / 4 ? th : (tw * 3 + 3) / 4);
    h += h % 2;
    *sh = h;
    *sw = ((h * 4 / 3) + 15) / 16 * 16;
}

void stream_plan(int *mw, int *mh, int *lw, int *lh, int *photo) {
    int hf, vf, turn;
    flips_for(A.st.cam_rotate, A.st.mirror, &hf, &vf, &turn);
    int vw, vh, cw, ch;
    target_size("view", &vw, &vh);
    if (mode() == MODE_PHOTO) {
        *mw = PHOTO_W;
        *mh = PHOTO_H;
        sensor_size_for(vw, vh, turn, lw, lh);
        if (*lw > *mw) *lw = *mw;
        if (*lh > *mh) *lh = *mh;
        *photo = 1;
        return;
    }
    target_size("clip", &cw, &ch);
    *photo = 0;
    if (turn) {
        *mw = ch; *mh = cw; *lw = vh; *lh = vw;
    } else {
        *mw = cw; *mh = ch; *lw = vw; *lh = vh;
    }
}

static double live_fps(void) {
    if (mode() == MODE_PHOTO && A.S.live_fps >= 5 && A.S.live_fps <= 30) return A.S.live_fps;
    return VIDEO_FPS;
}

/* (v53) The live view's speed: up to 30 pictures a second (was 15) - smoother,
 * and each picture on the screen sooner.
 * PHOTO mode: the camera is given a range - at least 1/30 s per picture, at
 * most 1/15 s - so it still exposes as long as before when it's dark (then it
 * slows down to 15 by itself): photos come out the same. Looks with more work
 * per picture get 24 or 20 a second, or stay at 15, so the Pi keeps time to
 * spare.
 * (v54) VIDEO mode too: videos keep 15 pictures a second - every other one
 * when the camera runs at 30 (rec_takes) - so the camera runs at exactly 30
 * (in good light: 1/30 s is long enough) or 15, nothing in between; 30 only
 * when the Pi has time for it, the video's own work included.
 * 15 a second (as before) in GIF mode, while saving or developing, outside
 * the live view - and with "live_fps" in settings.txt. */
#define CAMD_FRAME_COST 0.011                /* the camera server's work per picture (s) */
static int rate_min = -1, rate_max = -1;     /* the picture times asked for (us); -1: not known */
static double rate_work;                     /* this look's work per picture (s), smoothed */
static int rate_look = -1;

static void rate_set(int lo, int hi) {
    if ((lo != rate_min || hi != rate_max) && cam_configured()) {
        cam_set_frame_limits(lo, hi);
        rate_min = lo;
        rate_max = hi;
    }
}

void live_rate(double work, int exposure_us) {
    int slow = (int)(1e6 / VIDEO_FPS), fast = (int)(1e6 / 30);
    int fixed = rec_kind() == 'g' || mode() == MODE_GIF || A.S.live_fps || A.saver.pending ||
                A.developer.pending || A.cam_missing ||
                (A.screen != SCR_CAMERA && A.screen != SCR_MENU);
    if (A.st.filter != rate_look) {
        rate_look = A.st.filter;
        rate_work = 0;
    }
    if (work > 0) rate_work = rate_work > 0 ? rate_work * 0.9 + work * 0.1 : work;
    if (fixed || rate_work <= 0) {
        rate_set(slow, slow);
        return;
    }
    double per = rate_work + CAMD_FRAME_COST;
    int cur = rate_min > 0 ? (int)lround(1e6 / rate_min) : 15;
    if (mode() == MODE_VIDEO) {
        double load = per * 30 + rec_writer_cost(A.st.filter) * VIDEO_FPS;
        int ok = load <= (cur == 30 ? 0.92 : 0.82);
        /* (light enough: the exposure fits in 1/30 s - at 30, the camera can't
         *  go longer: near 1/30 s means it would like to - then 15 again) */
        int bright = exposure_us <= 0 ? cur == 30 : exposure_us <= (cur == 30 ? 32500 : 30500);
        int lim = ok && bright ? fast : slow;
        rate_set(lim, lim);
        return;
    }
    static const int STEPS[] = {30, 24, 20};
    int want = slow;
    for (int i = 0; i < 3; i++) {
        double room = STEPS[i] > cur ? 0.70 : 0.85;        /* (faster: with time to spare) */
        if (per * STEPS[i] <= room) {
            want = (int)(1e6 / STEPS[i]);
            break;
        }
    }
    rate_set(want, slow);
}

void apply_camera(int force) {
    int mw, mh, lw, lh, photo, turn, hf, vf;
    stream_plan(&mw, &mh, &lw, &lh, &photo);
    flips_for(A.st.cam_rotate, A.st.mirror, &hf, &vf, &turn);
    int buffers = photo ? 4 : 6;
    int fps = (int)live_fps();
    int key[10] = {mw, mh, lw, lh, hf, vf, buffers, fps, 1, 0};
    A.cam_h = hf;
    A.cam_v = vf;
    A.cam_turn = turn;
    if (!force && !memcmp(key, A.cfg_key, sizeof key)) return;
    memcpy(A.cfg_key, key, sizeof key);
    if (!cam_ready() && !force) {                 /* (the camera isn't there yet: sent once it is) */
        cam_configure(mw, mh, lw, lh, hf, vf, buffers, fps, 0);
        return;
    }
    double t = mono();
    rate_min = rate_max = (int)(1e6 / fps);        /* (a new set-up: its own speed - see live_rate) */
    if (cam_configure(mw, mh, lw, lh, hf, vf, buffers, fps, cam_ready() ? 8 : 0) && cam_ready())
        logw("Camera set-up %dx%d + %dx%d did not finish (%s)", mw, mh, lw, lh, cam_error());
    else if (cam_ready())
        logi("Camera set to %dx%d + %dx%d%s%s (%.2f s)", mw, mh, lw, lh, hf ? " hflip" : "", vf ? " vflip" : "",
             mono() - t);
}

void instant_size(int look, int *w, int *h) {
    /* (v53: every look at the clip size - the Game Boy looks too, like on the
     *  screen; until v52 they came in whole steps of their 128-pixel grid,
     *  only 3 sizes) */
    (void)look;
    target_size("clip", w, h);
}

/* Videos with a look are saved the moment you stop ("instant"), at every clip
 * size (from v47): the look is applied while recording, at the live view's
 * size - what the screen shows - and enlarged smoothly to the clip size (the
 * pixel looks with sharp pixel edges). Nothing to develop afterwards. (Normal
 * videos: the hardware video encoder at the clip size, saved at once too.) */
int instant_video(int look) { return look != LOOK_NORMAL; }

/* ------------------------------------------------------------- live view */
/* the camera's planes cut to the picture's shape (even edges) */
typedef struct { Img y, u, v; } Planes;

static Planes crop_planes(const CamPlanes *p) {
    double a = picture_aspect();
    if (A.cam_turn) a = 1 / a;
    int pw = p->w, ph = p->h, x = 0, y = 0, w = pw, h = ph;
    if ((double)pw / ph > a + 0.01) {
        w = pyround(ph * a) & ~1;
        x = ((pw - w) / 2) & ~1;
    } else if ((double)pw / ph < a - 0.01) {
        h = pyround(pw / a) & ~1;
        y = ((ph - h) / 2) & ~1;
    }
    Planes o;
    o.y = img_view((uint8_t *)p->y + (size_t)y * p->stride + x, w, h, 1, p->stride);
    o.u = img_view((uint8_t *)p->u + (size_t)(y / 2) * (p->stride / 2) + x / 2, w / 2, h / 2, 1, p->stride / 2);
    o.v = img_view((uint8_t *)p->v + (size_t)(y / 2) * (p->stride / 2) + x / 2, w / 2, h / 2, 1, p->stride / 2);
    return o;
}

/* small 16-bit picture -> the screen, fitted (black bars) */

static void fit_geom(int w, int h, int *x, int *y, int *nw, int *nh) {
    double sc = fmin((double)A.W / w, (double)A.H / h);
    *nw = pyround(w * sc);
    *nh = pyround(h * sc);
    if (*nw < 1) *nw = 1;
    if (*nh < 1) *nh = 1;
    *x = (A.W - *nw) / 2;
    *y = (A.H - *nh) / 2;
}

static void show_small(const uint16_t *px, int w, int h) {
    int x, y, nw, nh;
    fit_geom(w, h, &x, &y, &nw, &nh);
    if (x > 0 || y > 0) memset(A.scr.px, 0, (size_t)A.W * A.H * 2);
    cv_blit_565(&A.scr, px, w, h, x, y, nw, nh);
}

static uint16_t *rot565(const uint16_t *src, int w, int h) {    /* 90 degrees clockwise */
    uint16_t *o = malloc((size_t)w * h * 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) o[(size_t)x * h + (h - 1 - y)] = src[(size_t)y * w + x];
    return o;
}

static void show_rgb(Img rgb) {
    if (rgb.w == A.W && rgb.h == A.H && A.scr.w == A.W) {     /* (fills the screen: straight in) */
        for (int y = 0; y < rgb.h; y++) {
            const uint8_t *p = PX(rgb, 0, y);
            uint16_t *o = A.scr.px + (size_t)y * A.W;
            for (int x = 0; x < rgb.w; x++, p += 3) o[x] = C565(p[0], p[1], p[2]);
        }
        return;
    }
    uint16_t *px = malloc((size_t)rgb.w * rgb.h * 2);
    for (int y = 0; y < rgb.h; y++) {
        const uint8_t *p = PX(rgb, 0, y);
        for (int x = 0; x < rgb.w; x++, p += 3) px[y * rgb.w + x] = C565(p[0], p[1], p[2]);
    }
    show_small(px, rgb.w, rgb.h);
    free(px);
}

/* one row of Y, U, V (U, V half size) -> 16-bit screen pixels through a
 * yuv565_table (v55: two pixels per U, V - their part worked out once) */
static inline void yuv565_row(uint16_t *o, const uint8_t *py, const uint8_t *pu, const uint8_t *pv,
                              int w, const uint16_t *t) {
    int x = 0;
    for (; x + 1 < w; x += 2) {
        unsigned c = (unsigned)(pu[x >> 1] >> 3) << 5 | (pv[x >> 1] >> 3);
        o[x] = t[(unsigned)(py[x] >> 2) << 10 | c];
        o[x + 1] = t[(unsigned)(py[x + 1] >> 2) << 10 | c];
    }
    if (x < w) o[x] = t[(py[x] >> 2) << 10 | (pu[x >> 1] >> 3) << 5 | (pv[x >> 1] >> 3)];
}

/* (v55) Y, U, V (U, V half size) -> the screen, through a yuv565_table */
static void show_yuv(Img Y, Img U, Img V, const uint16_t *t) {
    int w = Y.w, h = Y.h, full = w == A.W && h == A.H && A.scr.w == A.W;
    uint16_t *px = full ? A.scr.px : malloc((size_t)w * h * 2);
    for (int j = 0; j < h; j++) {
        const uint8_t *py = PX(Y, 0, j), *pu = PX(U, 0, j / 2), *pv = PX(V, 0, j / 2);
        uint16_t *o = px + (size_t)j * w;
        yuv565_row(o, py, pu, pv, w, t);
    }
    if (!full) {
        show_small(px, w, h);
        free(px);
    }
}

static void show_index(Img idx, const uint16_t *pal) {
    if (idx.w == A.W && idx.h == A.H && A.scr.w == A.W) {     /* (fills the screen: straight in) */
        for (int y = 0; y < idx.h; y++) {
            const uint8_t *p = PX(idx, 0, y);
            uint16_t *o = A.scr.px + (size_t)y * A.W;
            for (int x = 0; x < idx.w; x++) o[x] = pal[p[x]];
        }
        return;
    }
    uint16_t *px = malloc((size_t)idx.w * idx.h * 2);
    for (int y = 0; y < idx.h; y++) {
        const uint8_t *p = PX(idx, 0, y);
        for (int x = 0; x < idx.w; x++) px[y * idx.w + x] = pal[p[x]];
    }
    show_small(px, idx.w, idx.h);
    free(px);
}

/* R,G,B of the (cut) camera planes at the view size, turned (frame_from) -
 * in one pass (img_view_rgb) */
static Img view_rgb(Planes pl) {
    int tw, th;
    target_size("view", &tw, &th);
    return img_view_rgb(pl.y, pl.u, pl.v, A.cam_turn, tw, th);
}

void live_item_free(LiveItem *it) {
    if (it->kind == 1) tone_free(&it->tone);
    if (it->kind == 2) img_free(&it->rgb);
    if (it->kind == 3) { img_free(&it->y); img_free(&it->u); img_free(&it->v); }
    it->kind = 0;
}

void draw_live(const CamFrame *f, void *item_out) {
    LiveItem *item = item_out;
    const CamPlanes *src = f->lores.y ? &f->lores : &f->main;
    int look = A.st.filter;
    int live = rec_live();
    cam_begin_read(src);
    Planes cut = crop_planes(src);
    /* copy out of the camera's memory (uncached: read it once) - the
     * brightness looks only need Y */
    int gray = look_is_tone(look);
    Planes pl = {img_copy(cut.y), gray ? (Img){0} : img_copy(cut.u), gray ? (Img){0} : img_copy(cut.v)};
    cam_end_read(src);
    if (look_is_yuv(look) && !live) {              /* Normal, Cross: one table */
        const uint16_t *t = yuv565_table(look);
        int w = pl.y.w, h = pl.y.h;
        uint16_t *px = malloc((size_t)w * h * 2);
        for (int y = 0; y < h; y++) {
            const uint8_t *py = PX(pl.y, 0, y), *pu = PX(pl.u, 0, y / 2), *pv = PX(pl.v, 0, y / 2);
            uint16_t *o = px + (size_t)y * w;
            yuv565_row(o, py, pu, pv, w, t);
        }
        if (A.cam_turn) {
            uint16_t *r = rot565(px, w, h);
            free(px);
            px = r;
            int t2 = w; w = h; h = t2;
        }
        show_small(px, w, h);
        free(px);
    } else if (look_is_pixel(look)) {              /* 16-Bit: palette numbers on the grid */
        Tone tn = pixel_tone_planes(look, pl.y, pl.u, pl.v, 0, 0);
        if (A.cam_turn) {
            Img r = img_rot90cw(tn.t);
            img_free(&tn.t);
            tn.t = r;
            int t2 = tn.w; tn.w = tn.h; tn.h = t2;
        }
        show_index(tn.t, palette_565(tn.pal));
        if (live && item) {
            item->kind = 1;
            item->tone = tn;
            tn.t.p = NULL;
        }
        if (tn.t.p) tone_free(&tn);
    } else if (look == LOOK_ZEROICHI) {             /* (v52) colour film: on Y, U, V - no R,G,B */
        int tw, th;
        target_size("view", &tw, &th);
        Img y = img_view_plane(pl.y, A.cam_turn, tw, th);
        Img u = img_view_plane(pl.u, A.cam_turn, tw / 2, th / 2), v = img_view_plane(pl.v, A.cam_turn, tw / 2, th / 2);
        Img yo = zi_luma(y);
        const uint16_t *t = yuv565_table(look);     /* (the colours are in the table) */
        uint16_t *px = malloc((size_t)tw * th * 2);
        for (int j = 0; j < th; j++) {
            const uint8_t *py = PX(yo, 0, j), *pu = PX(u, 0, j / 2), *pv = PX(v, 0, j / 2);
            uint16_t *o = px + (size_t)j * tw;
            yuv565_row(o, py, pu, pv, tw, t);
        }
        show_small(px, tw, th);
        free(px);
        if (live && item) {                        /* recording: the frame as Y, U, V */
            Img T = img_copy(y);
            img_point(T, T_ZI_TONE);
            Img h2 = (T.w == 2 * u.w && T.h == 2 * u.h) ? img_reduce(T, 2) : img_resize(T, u.w, u.h, RS_NEAREST);
            img_free(&T);
            item->kind = 3;
            zi_colour(h2, u, v, &item->u, &item->v);
            img_free(&h2);
            item->y = yo;
            yo.p = NULL;
        }
        if (yo.p) img_free(&yo);
        img_free(&y); img_free(&u); img_free(&v);
    } else if (look == LOOK_HOLGA_COLOR) {          /* (v55) on Y, U, V too - no R,G,B */
        int tw, th;
        target_size("view", &tw, &th);
        Img y = img_view_plane(pl.y, A.cam_turn, tw, th);
        Img u = img_view_plane(pl.u, A.cam_turn, tw / 2, th / 2), v = img_view_plane(pl.v, A.cam_turn, tw / 2, th / 2);
        Img yo, uo, vo;
        hc_yuv(y, u, v, &yo, &uo, &vo);
        img_free(&y); img_free(&u); img_free(&v);
        show_yuv(yo, uo, vo, yuv565_table(LOOK_NORMAL));
        if (live && item) {                        /* recording: the frame as Y, U, V */
            item->kind = 3;
            item->y = yo; item->u = uo; item->v = vo;
        } else {
            img_free(&yo); img_free(&uo); img_free(&vo);
        }
    } else if (gray) {                             /* black & white looks: brightness only */
        int tw, th;
        target_size("view", &tw, &th);
        Img g = img_view_plane(pl.y, A.cam_turn, tw, th);
        Tone tn = tone_make(look, g, 0, 0);
        img_free(&g);
        show_index(tn.t, palette_565(tn.pal));
        if (live && item) {
            item->kind = 1;
            item->tone = tn;
        } else {
            tone_free(&tn);
        }
    } else {                                       /* Super 8, Prism (and Cross for videos) */
        Img rgb = view_rgb(pl);
        Img out = look_rgb(look, rgb, 0, 0);
        img_free(&rgb);
        show_rgb(out);
        if (live && item) {
            item->kind = 2;
            item->rgb = out;
        } else {
            img_free(&out);
        }
    }
    img_free(&pl.y);
    img_free(&pl.u);
    img_free(&pl.v);
}

/* ------------------------------------------------------------- overlays */
void show_toast(const char *text, double seconds) {
    snprintf(A.toast, sizeof A.toast, "%s", text);
    A.toast_until = mono() + seconds;
    A.toast_secs = seconds;
}

/* (v52) Zero Ichi's design: a cream card with a red edge, ink letters */
static void draw_toast(void) {
    const Font *f = brand_font(15, 0);
    char lines[8][96];
    int n = 0;
    char text[160];
    snprintf(text, sizeof text, "%s", A.toast);
    for (char *ln = strtok(text, "\n"); ln && n < 8; ln = strtok(NULL, "\n")) {
        char cur[96] = "";
        char *save;
        for (char *w = strtok_r(ln, " ", &save); w; w = strtok_r(NULL, " ", &save)) {
            char test[200];
            snprintf(test, sizeof test, "%s%s%s", cur, cur[0] ? " " : "", w);
            if (cur[0] && text_width(f, test) > A.W - 52) {
                if (n < 8) snprintf(lines[n++], sizeof lines[0], "%s", cur);
                snprintf(cur, sizeof cur, "%s", w);
            } else {
                snprintf(cur, sizeof cur, "%s", test);
            }
        }
        if (cur[0] && n < 8) snprintf(lines[n++], sizeof lines[0], "%s", cur);
    }
    if (!n) return;
    double mw = 0;
    for (int i = 0; i < n; i++) mw = fmax(mw, text_width(f, lines[i]));
    int w = (int)mw + 40, h = 24 * n + 16;
    if (w > A.W - 8) w = A.W - 8;
    int x0 = (A.W - w) / 2, y0 = (A.H - h) / 2;
    cv_rect(&A.scr, x0, y0, x0 + w - 1, y0 + h - 1, ZI_CREAM);
    cv_rect(&A.scr, x0, y0, x0 + 5, y0 + h - 1, ZI_RED);
    for (int i = 0; i < n; i++) cv_text_f(&A.scr, f, x0 + 6 + (w - 6) / 2.0, y0 + 20 + 24 * i, lines[i], ZI_INK, "mm");
}

static const char *menu_value(int i, char *buf, size_t cap);

typedef struct { const char *key, *label; } MenuRow;
/* (0.1.0) the screen settings next to Rotation; "Size" is "Image ratio" */
static const MenuRow MENU[] = {
    {"presets", "Presets"}, {"cam_rotate", "Rotation"}, {"screen", "Screen"},
    {"screen_flip", "Flip screen"}, {"mirror", "Mirror"}, {"size", "Image ratio"},
    {"clip_size", "Clip size"}, {"gif_frames", "GIF frames"}, {"video_max", "Video max"},
    {"screensaver", "Screen saver"}, {"photospace", "Photo space"}, {"erase", "Erase all"},
    {"key3", "KEY3 hold"}, {"poweroff", "Power off"}, {"about", "About"}};
#define NMENU ((int)(sizeof MENU / sizeof MENU[0]))

/* the rows shown (Photo space only while the card doesn't use all its room) */
static int vis[NMENU], nvis;

void menu_refresh(void) {
    int space = photo_space_needed();
    nvis = 0;
    for (int i = 0; i < NMENU; i++)
        if (space || strcmp(MENU[i].key, "photospace")) vis[nvis++] = i;
    if (A.menu_idx >= nvis) A.menu_idx = 0;
    presets_refresh();
}

int menu_count(void) {
    if (!nvis) menu_refresh();
    return nvis;
}
const char *menu_key(int i) { return MENU[vis[i]].key; }
int menu_is_action(int i) {
    const char *k = menu_key(i);
    return !strcmp(k, "presets") || !strcmp(k, "about") || !strcmp(k, "photospace") || !strcmp(k, "erase") ||
           !strcmp(k, "poweroff");
}

static const char *menu_value(int i, char *buf, size_t cap) {
    const char *k = menu_key(i);
    State *s = &A.st;
    if (!strcmp(k, "presets")) snprintf(buf, cap, "%s", presets_label());
    else if (!strcmp(k, "cam_rotate")) snprintf(buf, cap, "%d°", s->cam_rotate);
    else if (!strcmp(k, "size")) snprintf(buf, cap, "%s", ratio_label(s->size));
    else if (!strcmp(k, "mirror")) snprintf(buf, cap, "%s", s->mirror ? "On" : "Off");
    else if (!strcmp(k, "screen_flip")) snprintf(buf, cap, "%s", s->screen_flip ? "On" : "Off");
    else if (!strcmp(k, "clip_size")) snprintf(buf, cap, "%s", s->clip_size);
    else if (!strcmp(k, "gif_frames")) snprintf(buf, cap, "%d", s->gif_frames);
    else if (!strcmp(k, "video_max")) {
        int m = s->video_max;
        if (m <= 0) snprintf(buf, cap, "No limit");
        else if (m < 60) snprintf(buf, cap, "%ds", m);
        else snprintf(buf, cap, "%d min", m / 60);
    }
    else if (!strcmp(k, "screen")) snprintf(buf, cap, "%dx%d", A.W, A.H);
    else if (!strcmp(k, "screensaver")) snprintf(buf, cap, "%s", s->screensaver);
    else if (!strcmp(k, "photospace")) snprintf(buf, cap, "%s", photo_space_label());
    else if (!strcmp(k, "key3")) snprintf(buf, cap, "%s", s->key3);
    else if (!strcmp(k, "poweroff")) snprintf(buf, cap, "Now");
    else if (!strcmp(k, "erase")) snprintf(buf, cap, A.count == 1 ? "1 file" : "%d files", A.count);
    else snprintf(buf, cap, "v%s", VERSION);
    return buf;
}

/* (v52) The menus in Zero Ichi's design: a cream card (the live picture
 * still shows around it), the little "0 1" mark and the title, a rule, ink
 * rows, the chosen row as an ink bar with a red dot. Drawn once and kept: each
 * frame only copies it (quicker than the old see-through panel). */
static uint16_t *card_px;
static int card_w, card_h;
static char card_key[2048];

/* the kept card if it is still right (1), else 0: draw it, then ui_card_keep() */
int ui_card_reuse(const char *key) {
    if (!card_px || card_w != A.W - 12 || card_h != A.H - 12 || strcmp(key, card_key)) return 0;
    cv_blit_565(&A.scr, card_px, card_w, card_h, 6, 6, card_w, card_h);
    return 1;
}

void ui_card_keep(const char *key) {
    if (!card_px || card_w != A.W - 12 || card_h != A.H - 12) {
        free(card_px);
        card_w = A.W - 12;
        card_h = A.H - 12;
        card_px = malloc((size_t)card_w * card_h * 2);
    }
    for (int y = 0; y < card_h; y++) memcpy(card_px + (size_t)y * card_w, A.scr.px + (size_t)(y + 6) * A.W + 6, (size_t)card_w * 2);
    snprintf(card_key, sizeof card_key, "%s", key);
}

/* the card and its header: the mark, the title, the logo's yellow quarter and
 * blue square, the rule (up: more rows above - a small red triangle) */
void ui_card(const char *title, int up) {
    int W = A.W, H = A.H;
    cv_rect(&A.scr, 6, 6, W - 7, H - 7, ZI_CREAM);
    ui_mark(&A.scr, 21, 21, 8, ZI_CREAM);
    cv_text_f(&A.scr, brand_font(16, 1), 43, 21.5, title, ZI_INK, "lm");
    ui_quarter(&A.scr, W - 34, 29, 9, 180, ZI_YEL);
    ui_rect(&A.scr, W - 30, 16, W - 22, 24, ZI_BLU);
    if (up) {
        float t[6] = {W - 52.f, 24, W - 46.f, 24, W - 49.f, 18};
        ui_poly(&A.scr, t, 3, ZI_RED);
    }
    ui_rect(&A.scr, 14, 34, W - 14, 35.5f, ZI_INK);
}

/* one row: label left, value right; chosen: the ink bar, a red dot */
void ui_row(int y, const char *label, const char *value, int chosen, Rgb label_col, Rgb value_col) {
    const Font *f = brand_font(14, 0);
    if (chosen) {
        cv_rect(&A.scr, 10, y - 10, A.W - 11, y + 10, ZI_INK);
        ui_circle(&A.scr, 17, y + 0.5f, 3.2f, ZI_RED);
    }
    cv_text_f(&A.scr, f, 26, y + 0.5, label, chosen ? ZI_CREAM : label_col, "lm");
    if (value && value[0]) cv_text_f(&A.scr, f, A.W - 18, y + 0.5, value, chosen ? ZI_YEL : value_col, "rm");
}

/* the bottom line (grey, small); more: rows below - a small red triangle */
void ui_hint(const char *text, int more) {
    cv_text_f(&A.scr, brand_font(11, 0), 14, A.H - 16, text, ZI_GREY, "lm");
    if (more) {
        float t[6] = {A.W - 22.f, A.H - 19.f, A.W - 14.f, A.H - 19.f, A.W - 18.f, A.H - 13.f};
        ui_poly(&A.scr, t, 3, ZI_RED);
    }
}

static void draw_menu(void) {
    if (A.presets_open) {
        presets_draw();
        return;
    }
    int n = menu_count();
    if (A.menu_top < A.menu_idx - MENU_ROWS + 1) A.menu_top = A.menu_idx - MENU_ROWS + 1;
    if (A.menu_top > A.menu_idx) A.menu_top = A.menu_idx;
    char key[2048], vals[MENU_ROWS][64];
    int k = snprintf(key, sizeof key, "menu %d %d %d %d", A.menu_idx, A.menu_top, n, A.W);
    for (int i = A.menu_top; i < A.menu_top + MENU_ROWS && i < n; i++) {
        menu_value(i, vals[i - A.menu_top], sizeof vals[0]);
        k += snprintf(key + k, sizeof key - k, "|%s", vals[i - A.menu_top]);
        if (k >= (int)sizeof key) k = sizeof key - 1;
    }
    if (ui_card_reuse(key)) return;
    ui_card("menu", A.menu_top > 0);
    const Font *f = brand_font(14, 0);
    int y = 49;
    for (int i = A.menu_top; i < A.menu_top + MENU_ROWS && i < n; i++, y += 23) {
        const char *val = vals[i - A.menu_top];
        char shown[80];
        if (i == A.menu_idx && !menu_is_action(i)) {
            snprintf(shown, sizeof shown, "\xe2\x80\xb9 %s \xe2\x80\xba", val);         /* ‹ value › */
            char both[160];
            snprintf(both, sizeof both, "%s%s", MENU[vis[i]].label, shown);
            if (text_width(f, both) > A.W - 52) snprintf(shown, sizeof shown, "%s", val);
        } else {
            snprintf(shown, sizeof shown, "%s", val);
        }
        ui_row(y, MENU[vis[i]].label, shown, i == A.menu_idx, ZI_INK, ZI_BLU);
    }
    ui_hint(menu_is_action(A.menu_idx) ? "PRESS: do it" : "PRESS: close", A.menu_top + MENU_ROWS < n);
    ui_card_keep(key);
}

/* how much still fits on the card (bottom left, second row) */
static void space_text(char *lng, char *shrt, int *low) {
    lng[0] = shrt[0] = 0;
    *low = 0;
    free_space_update(0);
    long long fr = A.free_b;
    if (fr < 0) return;
    if (fr < MIN_SPACE) {
        strcpy(lng, "Card full");
        strcpy(shrt, "Card full");
        *low = 1;
        return;
    }
    long long usable = fr - MIN_SPACE;
    int look = A.st.filter;
    const char *kind = mode() == MODE_PHOTO ? "photo" : mode() == MODE_GIF ? "gif" : "video";
    char key[64];
    snprintf(key, sizeof key, "%s:%s", kind, LOOK_NAMES[look]);
    double bpp = state_space(&A.st, key);
    if (bpp <= 0) {
        double def = mode() == MODE_PHOTO ? 0.20 : mode() == MODE_GIF ? 0.50 : 0.14;
        double f = look == LOOK_GB || look == LOOK_GB_BW ? 0.25 : look_is_pixel(look) ? 0.5 : 1.0;
        bpp = def * f;
    }
    *low = fr < LOW_SPACE;
    int w, h;
    if (mode() == MODE_PHOTO) {
        target_size("photo", &w, &h);
        long long n = (long long)(usable / fmax(1.0, bpp * w * h));
        if (n >= 10000) snprintf(lng, 64, "%lldk photos left", n / 1000), snprintf(shrt, 32, "%lldk left", n / 1000);
        else snprintf(lng, 64, "%lld photos left", n), snprintf(shrt, 32, "%lld left", n);
        return;
    }
    target_size("clip", &w, &h);
    if (mode() == MODE_GIF) {
        long long n = (long long)(usable / fmax(1.0, bpp * w * h * A.st.gif_frames));
        if (n >= 10000) snprintf(lng, 64, "%lldk GIFs left", n / 1000), snprintf(shrt, 32, "%lldk left", n / 1000);
        else snprintf(lng, 64, "%lld GIFs left", n), snprintf(shrt, 32, "%lld left", n);
        return;
    }
    if (instant_video(look)) instant_size(look, &w, &h);
    long long s = (long long)(usable / fmax(1.0, bpp * w * h * VIDEO_FPS));
    long long m = s / 60;
    char t[32];
    if (m >= 60) snprintf(t, sizeof t, "%lldh%02lldm", m / 60, m % 60);
    else if (m >= 1) snprintf(t, sizeof t, "%lld min", m);
    else snprintf(t, sizeof t, "%lld s", s);
    snprintf(lng, 64, "%s left", t);
    snprintf(shrt, 32, "%s", t);
}

static void draw_hud(double now) {
    State *s = &A.st;
    int recording = rec_kind() != 0;
    char t[96];
    snprintf(t, sizeof t, "<  %s  >", LOOK_NAMES[s->filter]);
    cv_text(&A.scr, 18, A.W / 2, 14, t, WHITE, "mm", 1);
    char label[48] = "", ex[48] = "", right2[48] = "";
    int has_label = 0, has_ex = 0;
    if (!recording) {
        has_label = 1;
        snprintf(label, sizeof label, "%s", s->mode);
        int w, h;
        if (mode() == MODE_VIDEO && instant_video(s->filter)) {
            instant_size(s->filter, &w, &h);
            snprintf(label + strlen(label), sizeof label - strlen(label), " %dx%d", w, h);
        } else if (mode() != MODE_PHOTO) {
            target_size("clip", &w, &h);
            snprintf(label + strlen(label), sizeof label - strlen(label), " %dx%d", w, h);
        } else if (fabs(picture_aspect() - (double)A.W / A.H) > 1e-9) {
            strcat(label, look_square(s->filter) ? " 6x6" : picture_aspect() > 1 ? " 4:3" : " 1:1");
        }
        if (s->ev != 0) snprintf(ex, sizeof ex, "EV%+.1f", s->ev);
        has_ex = ex[0] != 0;
    }
    char cnt[24];
    snprintf(cnt, sizeof cnt, "#%d", A.count);
    double cw = cv_text_width(14, cnt);
    int ex_fits = has_ex && cv_text_width(14, label) + cv_text_width(14, ex) + cw + 34 <= A.W;
    double dp = A.dev_progress;
    if (dp >= 0) snprintf(right2, sizeof right2, "DEVELOPING %d%%", (int)(dp * 100) / 5 * 5);
    else if (A.saver.pending) snprintf(right2, sizeof right2, "SAVING");
    else if (has_ex && !ex_fits) snprintf(right2, sizeof right2, "%s", ex);
    if (has_label) cv_text(&A.scr, 14, 6, A.H - 14, label, MODE_COLORS[mode()], "lm", 1);
    if (has_ex && ex_fits) cv_text(&A.scr, 14, (int)(A.W - 6 - cw - 10), A.H - 14, ex, YELLOW, "rm", 1);
    if (right2[0]) cv_text(&A.scr, 14, A.W - 6, A.H - 32, right2, YELLOW, "rm", 1);
    cv_text(&A.scr, 14, A.W - 6, A.H - 14, cnt, WHITE, "rm", 1);
    /* space left */
    char lng[64], shrt[32];
    int low;
    space_text(lng, shrt, &low);
    if (lng[0]) {
        double room = A.W - 12 - (right2[0] ? cv_text_width(14, right2) + 12 : 0);
        cv_text(&A.scr, 14, 6, A.H - 32, cv_text_width(14, lng) <= room ? lng : shrt,
                low ? (Rgb){255, 90, 90} : (Rgb){205, 205, 215}, "lm", 1);
    }
    if (A.cam_missing) {
        cv_text(&A.scr, 22, A.W / 2, A.H / 2 - 14,
                cam_failed() ? "No camera found" : cam_searching() ? "Looking for the camera" : "Starting camera...",
                (Rgb){255, 120, 120}, "mm", 1);
        if (cam_failed() || cam_searching()) {
            cv_text(&A.scr, 14, A.W / 2, A.H / 2 + 14, "check the ribbon cable", WHITE, "mm", 1);
            cv_text(&A.scr, 14, A.W / 2, A.H / 2 + 34, "still trying...", (Rgb){190, 190, 210}, "mm", 1);
        }
    }
    if (recording) {
        char rl[64];
        rec_label(rl, sizeof rl, now);
        cv_text(&A.scr, 14, 6, A.H - 16, rl, REC_RED, "lm", 1);
        int n = (int)(A.W * fmin(1.0, rec_progress(now)));
        if (n > 0) cv_rect(&A.scr, 0, A.H - 4, n - 1, A.H - 1, REC_RED);
    }
}

/* the red dot (top right): the SD card is being written (saving, recording)
 * - don't unplug now. No dot: unplugging is safe (see sd_init) */
void draw_sd_dot(void) {
    if (!sd_writable()) return;
    cv_circle(&A.scr, A.W - 9, 9, 6, (Rgb){0, 0, 0});
    cv_circle(&A.scr, A.W - 9, 9, 4, (Rgb){255, 40, 40});
}

/* (v55) changing Rotation or Mirror in the MENU: the picture itself for a
 * moment (the menu's card would hide it), the choice in a small card at the
 * bottom; up, down or press: the MENU again (v54: the white balance) */
static void draw_peek(void) {
    char t[64], v[32], note[32];
    menu_value(A.menu_idx, v, sizeof v);
    snprintf(t, sizeof t, "\xe2\x80\xb9  %s  \xe2\x80\xba", v);
    snprintf(note, sizeof note, "%s", MENU[vis[A.menu_idx]].label);
    note[0] = (char)tolower((unsigned char)note[0]);
    const Font *fl = brand_font(12, 0), *fv = brand_font(16, 1);
    int w = (int)fmax(text_width(fv, t), text_width(fl, note)) + 40, h = 50;
    int x0 = (A.W - w) / 2, y0 = A.H - h - 12;
    cv_rect(&A.scr, x0, y0, x0 + w - 1, y0 + h - 1, ZI_CREAM);
    cv_rect(&A.scr, x0, y0, x0 + 5, y0 + h - 1, ZI_RED);
    cv_text_f(&A.scr, fl, x0 + 6 + (w - 6) / 2.0, y0 + 15, note, ZI_GREY, "mm");
    cv_text_f(&A.scr, fv, x0 + 6 + (w - 6) / 2.0, y0 + 34, t, ZI_INK, "mm");
}

void draw_overlays(double now) {
    if (A.screen == SCR_MENU && now < A.peek_until && !A.presets_open) draw_peek();
    else if (A.screen == SCR_MENU) draw_menu();
    else draw_hud(now);
    draw_sd_dot();
    if (A.toast[0]) {
        if (now < A.toast_until) draw_toast();
        else A.toast[0] = 0;
    }
}

/* one camera frame: live view + overlays + to the screen; recording */
void tick_camera_frame(const CamFrame *fp) {
    double now = mono();
    double cpu0 = thread_cpu();
    if (!fp) {
        /* starting: the start-up picture stays on until the live view is ready
         * (no "Starting camera..." in between) - unless the camera is missing
         * or very late */
        if (A.splash_hold && !cam_failed() && !cam_searching() && now - A.splash_since < 12) return;
        A.splash_hold = 0;
        if (A.cam_missing || !cam_configured()) {        /* no picture yet: say so */
            cv_fill(&A.scr, (Rgb){24, 24, 24});
            draw_overlays(now);
            lcd_show(A.scr.px);
        }
        return;
    }
    if (A.splash_hold) {                             /* the live view is here */
        A.splash_hold = 0;
        if (A.toast[0]) A.toast_until = now + A.toast_secs;   /* (a message from the start: shown now) */
    }
    CamFrame f = *fp;
    double t_cam = mono();
    LiveItem item = {0};
    draw_live(&f, &item);
    double t_filter = mono();
    draw_overlays(now);
    lcd_show(A.scr.px);
    if (rec_kind()) {
        rec_frame(&f, now, &item);                 /* (takes the frame and the item) */
        rec_note_frame_cpu(thread_cpu() - cpu0);
        live_rate(thread_cpu() - cpu0, f.exposure_us);
        return;
    }
    release_frame(f.cookie);
    live_item_free(&item);
    live_rate(thread_cpu() - cpu0, f.exposure_us);
    /* live view speed (logged every 30 s) - of the time the live view ran:
     * a pause (recording, the menu, a test, saving) is left out (v49) */
    double end = mono();
    if (A.lv_last > 0 && now - A.lv_last > 0.5) A.lv_t0 += now - A.lv_last;
    A.lv_last = end;
    A.lv_n++;
    A.lv_cam += t_cam - now;
    A.lv_filter += t_filter - t_cam;
    A.lv_screen += end - t_filter;
    A.lv_busy += end - now;
    A.lv_cpu += thread_cpu() - cpu0;
    if (now - A.lv_t0 >= 30 && A.lv_n) {
        int n = A.lv_n;
        char cpu[256], health[96];
        cpu_report(cpu, sizeof cpu, 1);
        int vw, vh;
        target_size("view", &vw, &vh);
        logi("Live view: %.1f frames/s; per frame: filter %.0f ms, screen %.0f ms, own work %.0f ms "
             "[%s, %dx%d, %s]; CPU: %s; Pi: %s",
             n / fmax(now - A.lv_t0, 0.01), 1000 * A.lv_filter / n, 1000 * A.lv_screen / n,
             1000 * A.lv_cpu / n, LOOK_NAMES[A.st.filter], vw, vh, A.st.mode, cpu,
             pi_health(health, sizeof health));
        A.lv_n = 0;
        A.lv_cam = A.lv_filter = A.lv_screen = A.lv_busy = A.lv_cpu = 0;
        A.lv_t0 = now;
    }
}

/* the camera's newest frame to the screen; while the hardware encoder
 * records, every frame goes to it (the screen shows the newest) */
void camera_step(void) {
    if (rec_hw()) {
        CamFrame fr[CAM_MAXREQ];
        int n = cam_take_all(fr, CAM_MAXREQ);
        for (int i = 0; i + 1 < n; i++) rec_extra_frame(&fr[i]);
        tick_camera_frame(n ? &fr[n - 1] : NULL);
        return;
    }
    CamFrame f;
    if (!cam_take(&f)) {
        tick_camera_frame(NULL);
        return;
    }
    /* (v49) while a photo or GIF is being saved: every other frame only - the
     * live view at half speed for that moment, the saving gets the time (a
     * Prism photo: about twice as quick) */
    static unsigned skip;
    if (A.saver.pending && !rec_active() && A.screen == SCR_CAMERA && (++skip & 1)) {
        release_frame(f.cookie);
        return;
    }
    tick_camera_frame(&f);
}

/* ------------------------------------------------------------- whole screens */
/* (v52) a message in Zero Ichi's design: cream, the little "0 1", the title
 * (lines up to the first empty one: big) and the text under it - drawn, not
 * shown (see show_message) */
void brand_lines(const char *const *lines, int n, Rgb title_col) {
    int W = A.W, H = A.H, nt = 0, nb = 0;
    while (nt < n && lines[nt][0]) nt++;
    for (int i = nt; i < n; i++) nb += lines[i][0] != 0;
    float r = 9, block = 2 * r + 12 + nt * 28 + (nb ? 4 + nb * 21 : 0);
    float y = (H - block) / 2;
    cv_fill(&A.scr, ZI_CREAM);
    ui_mark(&A.scr, W / 2.0f - 0.25f * r - 1.5f, y + r, r, ZI_CREAM);
    y += 2 * r + 12 + 14;                           /* (the middle of the first title line) */
    for (int i = 0; i < nt; i++, y += 28) {
        const Font *f = brand_font(22, 1);
        if (text_width(f, lines[i]) > W - 20) f = brand_font(16, 1);
        cv_text_f(&A.scr, f, W / 2.0, y, lines[i], title_col, "mm");
    }
    for (int i = nt; i < n; i++) {
        if (!lines[i][0]) continue;
        const Font *f = brand_font(14, 0);
        if (text_width(f, lines[i]) > W - 16) f = brand_font(12, 0);
        cv_text_f(&A.scr, f, W / 2.0, y, lines[i], !strncmp(lines[i], "KEY", 3) ? ZI_BLU : ZI_GREY, "mm");
        y += 21;
    }
}

void show_message(const char *title, const char *sub, Rgb bg) {
    const char *lines[3] = {title, "", sub ? sub : ""};
    brand_lines(lines, sub ? 3 : 1, bg.r > bg.g + 40 ? ZI_RED : ZI_INK);    /* (red ones: warnings) */
    lcd_show_now(A.scr.px);
}

/* a title (lines up to the first empty one) and text below */
void screen_lines(const char *const *lines, int n, Rgb title_color, Rgb bg) {
    cv_fill(&A.scr, bg);
    int i = 0, y = 26, wide = A.W >= 300;
    for (; i < n && lines[i][0]; i++, y += wide ? 28 : 26)
        cv_text(&A.scr, wide ? 22 : 18, A.W / 2, y, lines[i], title_color, "mm", 1);
    y += 6;
    for (; i < n; i++, y += 21)
        if (lines[i][0])
            cv_text(&A.scr, wide ? 16 : 14, A.W / 2, y, lines[i],
                    !strncmp(lines[i], "KEY", 3) ? YELLOW : (Rgb){220, 220, 230}, "mm", 1);
}

/* (v53) MENU > About: a page of its own in the start-up picture's design - the
 * mark and the name, this Zero Ichi's details, and who made it. Any key: back
 * to the MENU. (Python: app.py, about_image - the same page.) */
static void size_text(long long b, char *o, size_t n) {
    if (b < 0) snprintf(o, n, "-");
    else if (b >= (10LL << 30)) snprintf(o, n, "%.0f GB", b / 1073741824.0);
    else if (b >= (1LL << 30)) snprintf(o, n, "%.1f GB", b / 1073741824.0);
    else snprintf(o, n, "%.0f MB", b / 1048576.0);
}

void about_draw(void) {
    Canvas *c = &A.scr;
    int w = A.W, h = A.H, wide = w > h;
    float L = w * .125f, R = w * .875f;
    cv_fill(c, ZI_CREAM);                            /* (v54: plain cream - no thin lines) */
    float s = wide ? .40f : .34f, r = 60 * s, mx = L + r, my = 12 + r;
    ui_logo(c, mx, my, s, ZI_CREAM);
    float tx = mx - r + ui_logo_width(s) + (wide ? 16 : 10);
    const Font *title = brand_font(22, 1);
    if (tx + text_width(title, "zero ichi") > w - 6) title = brand_font(16, 1);
    cv_text_f(c, title, tx, my - 6, "zero ichi", ZI_INK, "lm");
    ui_art(c, &BR_KANA, (int)tx + 1, (int)(my + 8));
    float y = my + r + 12;
    char ver[16], scr[48], ready[24], count[16], fr[24];
    snprintf(ver, sizeof ver, "v%s", VERSION);
    snprintf(scr, sizeof scr, "%dx%d", w, h);
    if (A.ready_after > 0) snprintf(ready, sizeof ready, "%.1f s", A.ready_after);
    else snprintf(ready, sizeof ready, "-");
    snprintf(count, sizeof count, "%d", A.nmedia);
    size_text(A.photo_dir[0] ? A.free_b : -1, fr, sizeof fr);
    const char *rows[5][2] = {{"Version", ver}, {"Screen", scr}, {"Start-up time", ready},
                              {wide ? "Photos, GIFs & videos" : "Photos & videos", count}, {"Free space", fr}};
    const Font *fl = brand_font(12, 0), *fv = brand_font(wide ? 14 : 12, 0);
    float step = wide ? 21 : 19;
    y += 18;
    for (int i = 0; i < 5; i++, y += step) {
        cv_text_f(c, fl, L, y, rows[i][0], ZI_GREY, "lm");
        cv_text_f(c, fv, R, y, rows[i][1], ZI_INK, "rm");
    }
    /* (the wide screen: how to go back) */
    float fy = h - 19;
    if (wide) cv_text_f(c, brand_font(11, 0), R, fy, "any key: back", ZI_GREY, "rm");
}

void splash_screen(void) {                          /* (v51: splash*.rgb565 missing - the same design) */
    splash_draw(&A.scr, VERSION);
    lcd_show_now(A.scr.px);
}

void free_space_update(int force) {
    /* (free_at 0 = not measured yet / out of date: the clock counts from the
     * Pi's start, so "15 s since 0" isn't up during the first seconds) */
    double now = mono();
    if (!force && A.free_at > 0 && now - A.free_at < 15) return;
    A.free_b = A.photo_dir[0] ? free_bytes(A.photo_dir) : -1;    /* (-1: nothing shown) */
    A.free_at = A.photo_dir[0] ? now : 0;
}
