/* Zero Ichi - photos, GIFs and videos: taking, recording, saving, developing,
 * file names and the space they take (a port of app.py's media parts). */
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "app.h"
#include "gif.h"
#include "lcd.h"
#include "util.h"

/* ------------------------------------------------------------ file names */
static int media_number(const char *name) {
    if (strncmp(name, "IMG_", 4) && strncmp(name, "GIF_", 4) && strncmp(name, "VID_", 4)) return -1;
    const char *p = name + 4;
    int n = 0, digits = 0;
    while (isdigit((unsigned char)*p) && digits < 8) n = n * 10 + (*p++ - '0'), digits++;
    if (!digits || digits > 7 || *p != '.') return -1;
    return n;
}

static int media_cmp(const void *a, const void *b) {
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int nx = media_number(x), ny = media_number(y);
    if ((nx < 0) != (ny < 0)) return nx < 0 ? -1 : 1;         /* older dated names first */
    if (nx < 0) {
        int c = strcmp(x + 4, y + 4);
        return c ? c : strcmp(x, y);
    }
    return nx != ny ? (nx < ny ? -1 : 1) : strcmp(x, y);
}

static int is_media(const char *n) {
    if (strncmp(n, "IMG_", 4) && strncmp(n, "GIF_", 4) && strncmp(n, "VID_", 4)) return 0;
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".gif") || !strcasecmp(dot, ".mov"));
}

void list_media(void) {
    for (int i = 0; i < A.nmedia; i++) free(A.media[i]);
    free(A.media);
    A.media = NULL;
    A.nmedia = 0;
    DIR *d = opendir(A.photo_dir);
    if (!d) return;
    int cap = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_media(e->d_name)) continue;
        if (A.nmedia == cap) {
            cap = cap ? cap * 2 : 64;
            A.media = realloc(A.media, sizeof(char *) * cap);
        }
        A.media[A.nmedia++] = strdup(e->d_name);
    }
    closedir(d);
    if (A.nmedia) qsort(A.media, A.nmedia, sizeof(char *), media_cmp);
}

static int first_free_number(void) {
    int top = 0;
    DIR *d = opendir(A.photo_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            int n = media_number(e->d_name);
            if (n > top) top = n;
        }
        closedir(d);
    }
    return top + 1 > A.st.next_number ? top + 1 : A.st.next_number;
}

static pthread_mutex_t names_lock = PTHREAD_MUTEX_INITIALIZER;

static int reserved(const char *name) {
    for (int i = 0; i < 8; i++)
        if (A.reserved[i][0] && !strcmp(A.reserved[i], name)) return 1;
    return 0;
}

void new_media_path(const char *prefix, const char *ext, char *out, size_t cap) {
    pthread_mutex_lock(&names_lock);
    if (!A.next_num) A.next_num = first_free_number();
    int n = A.next_num;
    char name[32];
    for (;; n++) {
        snprintf(name, sizeof name, "%s_%04d%s", prefix, n, ext);
        int taken = reserved(name);
        static const char *const kinds[][2] = {{"IMG", ".jpg"}, {"GIF", ".gif"}, {"VID", ".mov"}, {"VID", ".part"}};
        for (int k = 0; k < 4 && !taken; k++) {
            char p[600];
            snprintf(p, sizeof p, "%s/%s_%04d%s", A.photo_dir, kinds[k][0], n, kinds[k][1]);
            taken = file_exists(p);
        }
        if (!taken) break;
    }
    A.next_num = n + 1;
    A.st.next_number = n + 1;
    for (int i = 0; i < 8; i++)
        if (!A.reserved[i][0]) {
            snprintf(A.reserved[i], sizeof A.reserved[i], "%s", name);
            break;
        }
    pthread_mutex_unlock(&names_lock);
    mark_dirty();
    snprintf(out, cap, "%s/%s", A.photo_dir, name);
}

void unreserve(const char *path) {
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;
    pthread_mutex_lock(&names_lock);
    for (int i = 0; i < 8; i++)
        if (!strcmp(A.reserved[i], name)) A.reserved[i][0] = 0;
    pthread_mutex_unlock(&names_lock);
}

/* ------------------------------------------------------------- results */
void add_result(const char *toast, double seconds, int counted, const char *kind, int look, double pixels,
                double bytes, const char *path) {
    pthread_mutex_lock(&A.res_lock);
    if (A.nresults < 16) {
        Result *r = &A.results[A.nresults++];
        memset(r, 0, sizeof *r);
        snprintf(r->toast, sizeof r->toast, "%s", toast ? toast : "");
        r->seconds = seconds;
        r->counted = counted;
        snprintf(r->learn_kind, sizeof r->learn_kind, "%s", kind ? kind : "");
        r->learn_look = look;
        r->pixels = pixels;
        r->bytes = bytes;
        snprintf(r->done_path, sizeof r->done_path, "%s", path ? path : "");
    }
    pthread_mutex_unlock(&A.res_lock);
}

void pump_results(void) {
    pthread_mutex_lock(&A.res_lock);
    int n = A.nresults;
    Result r[16];
    memcpy(r, A.results, sizeof(Result) * n);
    A.nresults = 0;
    pthread_mutex_unlock(&A.res_lock);
    for (int i = 0; i < n; i++) {
        if (r[i].done_path[0]) unreserve(r[i].done_path);
        if (r[i].learn_kind[0]) learn_space(r[i].learn_kind, r[i].learn_look, r[i].pixels, r[i].bytes);
        if (r[i].counted) A.count++;
        if (r[i].toast[0]) show_toast(r[i].toast, r[i].seconds);
    }
}

void learn_space(const char *kind, int look, double pixels, double bytes) {
    if (pixels <= 0 || bytes <= 0) return;
    char key[64];
    snprintf(key, sizeof key, "%s:%s", kind, LOOK_NAMES[look]);
    double v = bytes / pixels, old = state_space(&A.st, key);
    v = old > 0 ? old * 0.7 + v * 0.3 : v;
    state_set_space(&A.st, key, round(v * 10000) / 10000);
    A.free_at = 0;
    mark_dirty();
}

int card_full(void) {
    double now = mono();
    if (A.free_at <= 0 || now - A.free_at > 5) free_space_update(1);
    return A.free_b >= 0 && A.free_b < MIN_SPACE;
}

void wait_for_saves(const char *why) {
    if (A.saver.pending) {
        show_message(why, "one moment", (Rgb){20, 20, 28});
        worker_wait(&A.saver, 600);
        pump_results();
    }
}

/* ------------------------------------------------------------- photos */
typedef struct {
    Img y, u, v;                    /* the photo's part of the frame (not turned yet) */
    int turn, look, w, h;
    char path[512];
    double t;
    int exposure_us;
    double gain;
} PhotoJob;

static void save_photo(void *arg) {
    PhotoJob *j = arg;
    double t0 = mono();
    sd_hold();                                       /* (saving: the card writable - see sd_init) */
    Img Y = j->y, U = j->u, V = j->v;
    if (j->turn) {
        Img a = img_rot90cw(Y), b = img_rot90cw(U), c = img_rot90cw(V);
        img_free(&Y); img_free(&U); img_free(&V);
        Y = a; U = b; V = c;
    }
    if (Y.w != j->w || Y.h != j->h) {                 /* (only if the stream is smaller) */
        Img a = img_resize(Y, j->w, j->h, RS_BILINEAR), b = img_resize(U, j->w / 2, j->h / 2, RS_BILINEAR),
            c = img_resize(V, j->w / 2, j->h / 2, RS_BILINEAR);
        img_free(&Y); img_free(&U); img_free(&V);
        Y = a; U = b; V = c;
    }
    Img oy, ou, ov, rgb;
    int yuv = look_photo(j->look, Y, U, V, &oy, &ou, &ov, &rgb);
    img_free(&Y); img_free(&U); img_free(&V);
    double t_pic = mono();
    JpegMaker jm;
    Buf out = {0};
    int bad;
    if (yuv) {
        jm_init(&jm, oy.w, oy.h, JPEG_QUALITY, 3.0);
        bad = jm_yuv(&jm, oy.p, oy.stride, ou.p, ov.p, ou.stride, &out);
        img_free(&oy); img_free(&ou); img_free(&ov);
    } else {
        jm_init(&jm, rgb.w, rgb.h, JPEG_QUALITY, 3.0);
        bad = jm_rgb(&jm, rgb, &out);
        img_free(&rgb);
    }
    jm_close(&jm);
    double t_jpg = mono();
    if (!bad) {                                      /* (who made it, the look - see meta.h) */
        Buf ex = {0};
        jpeg_add_exif(out.p, out.n, LOOK_NAMES[j->look], &ex);
        buf_free(&out);
        out = ex;
        bad = write_file(j->path, out.p, out.n, 1);
    }
    const char *name = strrchr(j->path, '/') + 1;
    if (bad) {
        loge("Saving %s failed", name);
        unlink(j->path);
        add_result("Save failed!", 2, 0, NULL, 0, 0, 0, j->path);
    } else {
        logi("Saved %s in %.1f s (picture %.1f s, JPEG %.1f s %s %s, write %.1f s; exposure %.1f ms, gain %.2f)",
             name, mono() - t0, t_pic - t0, t_jpg - t_pic, yuv ? "yuv" : "img", jm_how(&jm),
             mono() - t_jpg, j->exposure_us / 1000.0, j->gain);
        add_result(NULL, 0, 1, "photo", j->look, (double)jm.w * jm.h, (double)out.n, j->path);
    }
    sd_release();
    buf_free(&out);
    free(j);
}

static void take_photo(void) {
    if (A.saver.pending >= 2) {                       /* two photos already waiting */
        show_message("Saving...", "one moment", (Rgb){20, 20, 28});
        while (A.saver.pending >= 2) msleep(50);
    }
    CamFrame f;
    if (!cam_wait_frame(&f, 2.0)) {
        show_toast("No camera!", 1.5);
        return;
    }
    PhotoJob *j = calloc(1, sizeof *j);
    target_size("photo", &j->w, &j->h);
    j->turn = A.cam_turn;
    j->look = A.st.filter;
    j->exposure_us = f.exposure_us;
    j->gain = f.gain;
    new_media_path("IMG", ".jpg", j->path, sizeof j->path);
    /* "flash" */
    cv_fill(&A.scr, WHITE);
    lcd_show(A.scr.px);
    /* the photo's part of the frame (centred), copied out of the camera buffer */
    const CamPlanes *m = &f.main;
    int sw = j->turn ? j->h : j->w, sh = j->turn ? j->w : j->h;
    if (sw > m->w) sw = m->w;
    if (sh > m->h) sh = m->h;
    sw &= ~1;
    sh &= ~1;
    int x0 = ((m->w - sw) / 2) & ~1, y0 = ((m->h - sh) / 2) & ~1;
    cam_begin_read(m);
    j->y = img_copy(img_view((uint8_t *)m->y + (size_t)y0 * m->stride + x0, sw, sh, 1, m->stride));
    j->u = img_copy(img_view((uint8_t *)m->u + (size_t)(y0 / 2) * (m->stride / 2) + x0 / 2, sw / 2, sh / 2, 1, m->stride / 2));
    j->v = img_copy(img_view((uint8_t *)m->v + (size_t)(y0 / 2) * (m->stride / 2) + x0 / 2, sw / 2, sh / 2, 1, m->stride / 2));
    cam_end_read(m);
    release_frame(f.cookie);
    worker_submit(&A.saver, save_photo, j);
    show_toast("Click!", 0.6);
    drain_events();
}

/* ------------------------------------------------------------- recording */
#define MAXQ 8
struct Rec {
    int kind;                       /* 'g' GIF, 'v' video */
    int live, hw, sw;               /* instant / hardware / software video */
    int look, turn;
    double start;
    /* GIF */
    Img *gy, *gu, *gv;
    double *gt;
    int n, target;
    /* video */
    char path[512], raw[512];
    MovWriter mov;
    Venc *venc;
    int vw, vh;                     /* video size */
    /* instant / software video: frames compressed by a writer thread */
    pthread_t th;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    void *q[MAXQ];
    double qt[MAXQ];
    int qn, quit, dropped, frames, error;
    double spent, spent_cpu;        /* the writer's time (and its own working time) */
    double main_cpu;                /* the live view's work for the recorded frames */
    int64_t due_ts;                 /* (v54) the camera's time of the video's next picture */
    int due_set;
    int64_t ts0;                    /* (v54) the camera's time of the video's first picture */
    double t0;
    int main_n, queue_max;
    JpegMaker jm;
    int shown;
    double cpu_t;
};

int rec_active(void) { return A.rec != NULL; }
int rec_kind(void) { return A.rec ? A.rec->kind : 0; }
int rec_hw(void) { return A.rec && A.rec->hw; }
int rec_live(void) { return A.rec && A.rec->live; }
double rec_start(void) { return A.rec ? A.rec->start : 0; }

double rec_progress(double now) {
    if (!A.rec) return 0;
    if (A.rec->kind == 'g') return A.rec->target ? (double)A.rec->n / A.rec->target : 0;
    return A.st.video_max > 0 ? (now - A.rec->start) / A.st.video_max : 0;
}

const char *rec_label(char *buf, size_t cap, double now) {
    if (A.rec && A.rec->kind == 'v') {
        int s = (int)(now - A.rec->start), m = A.st.video_max;
        if (m <= 0) snprintf(buf, cap, "● REC %d:%02d", s / 60, s % 60);
        else if (m < 60) snprintf(buf, cap, "● REC %d:%02d / %ds", s / 60, s % 60, m);
        else snprintf(buf, cap, "● REC %d:%02d / %d:%02d", s / 60, s % 60, m / 60, m % 60);
    } else {
        snprintf(buf, cap, "● GIF");
    }
    return buf;
}

/* fitted into bw x bh, centred, even numbers (app.py _fit_box) */
static void fit_box(int w, int h, int bw, int bh, int *fw, int *fh, int *x, int *y) {
    double s = fmin((double)bw / w, (double)bh / h);
    int a = pyround(w * s), b = pyround(h * s);
    a = (a < 2 ? 2 : a) & ~1;
    b = (b < 2 ? 2 : b) & ~1;
    *fw = a < bw ? a : bw;
    *fh = b < bh ? b : bh;
    *x = ((bw - *fw) / 2) & ~1;
    *y = ((bh - *fh) / 2) & ~1;
}

/* one frame for an instant or software video -> JPEG -> the video file */
typedef struct { int kind; Tone tone; Img rgb; Img y, u, v; } VItem;   /* kind 1 tone, 2 rgb, 3 yuv */

static void item_free(VItem *it) {
    if (it->kind == 1) tone_free(&it->tone);
    if (it->kind == 2) img_free(&it->rgb);
    if (it->kind == 3) { img_free(&it->y); img_free(&it->u); img_free(&it->v); }
}

/* the picture's size and place in the video (fit_box), from the item */
static void item_box(Rec *r, VItem *it, int *fw, int *fh, int *x, int *y) {
    int w = it->kind == 1 ? it->tone.w : it->kind == 2 ? it->rgb.w : it->y.w;
    int h = it->kind == 1 ? it->tone.h : it->kind == 2 ? it->rgb.h : it->y.h;
    fit_box(w, h, r->vw, r->vh, fw, fh, x, y);
}

/* (v50) the frame is made straight into the planes the JPEG encoder reads -
 * the hardware encoder's own buffer: no copying, much less memory traffic
 * (which slowed the live view while recording) */
static void encode_item(Rec *r, VItem *it) {
    int bw = r->vw, bh = r->vh, fw, fh, x, y;
    uint8_t *py, *pu, *pv;
    int ys, cs;
    if (jm_planes(&r->jm, &py, &ys, &pu, &pv, &cs)) {
        r->error = 1;
        item_free(it);
        return;
    }
    Img Yd = img_view(py, bw, bh, 1, ys), Ud = img_view(pu, bw / 2, bh / 2, 1, cs), Vd = img_view(pv, bw / 2, bh / 2, 1, cs);
    item_box(r, it, &fw, &fh, &x, &y);
    if (fw != bw || fh != bh) {                      /* another shape: black bars around it */
        for (int j = 0; j < bh; j++) {
            uint8_t *row = PX(Yd, 0, j);
            if (j < y || j >= y + fh) memset(row, 0, bw);
            else { memset(row, 0, x); memset(row + x + fw, 0, bw - x - fw); }
        }
        for (int j = 0; j < bh / 2; j++) {
            for (int k = 0; k < 2; k++) {
                uint8_t *row = PX(k ? Vd : Ud, 0, j);
                if (j < y / 2 || j >= y / 2 + fh / 2) memset(row, 128, bw / 2);
                else { memset(row, 128, x / 2); memset(row + x / 2 + fw / 2, 128, bw / 2 - x / 2 - fw / 2); }
            }
        }
    }
    Img Yt = img_sub(Yd, x, y, fw, fh), Ut = img_sub(Ud, x / 2, y / 2, fw / 2, fh / 2),
        Vt = img_sub(Vd, x / 2, y / 2, fw / 2, fh / 2);
    if (it->kind == 1) {
        Img Y, U, V;
        tone_yuv(&it->tone, fw, fh, 1, &Y, &U, &V);
        img_resize_into(Y, Yt, RS_NEAREST);          /* (the same size: copied) */
        img_resize_into(U, Ut, RS_NEAREST);
        img_resize_into(V, Vt, RS_NEAREST);
        img_free(&Y); img_free(&U); img_free(&V);
    } else if (it->kind == 2) {
        Img rgb = it->rgb;
        /* (the screen's picture: e.g. 320x240 -> a 640x480 video: its Y is
         *  twice the size - a quick exact 2x - and its full-size U, V are just
         *  right; other sizes: U, V at half size first (reduce 2), then
         *  enlarged like Y - see resample) */
        if (fw / 2 == rgb.w && fh / 2 == rgb.h) {
            Img y0 = img_alloc(rgb.w, rgb.h, 1);
            img_rgb_to_ycc_into(rgb, y0, Ut, Vt);
            img_resize_into(y0, Yt, RS_BILINEAR);
            img_free(&y0);
        } else if (fw == rgb.w && fh == rgb.h && !((fw | fh) & 1)) {
            img_rgb_to_yuv420_into(rgb, Yt, Ut, Vt);
        } else {
            Img y0, u0, v0;
            img_rgb_to_yuv420(rgb, &y0, &u0, &v0);
            img_resize_into(y0, Yt, RS_BILINEAR);
            img_resize_into(u0, Ut, RS_BILINEAR);
            img_resize_into(v0, Vt, RS_BILINEAR);
            img_free(&y0); img_free(&u0); img_free(&v0);
        }
    } else {
        img_resize_into(it->y, Yt, RS_BILINEAR);
        img_resize_into(it->u, Ut, RS_BILINEAR);
        img_resize_into(it->v, Vt, RS_BILINEAR);
    }
    item_free(it);
    Buf out = {0};
    if (jm_encode_planes(&r->jm, &out) == 0) {
        if (mov_add(&r->mov, out.p, out.n, 0) == 0) r->frames++;
    } else {
        r->error = 1;
    }
    buf_free(&out);
}

/* (v50) The writer gives way to the live view: a lower priority while it
 * keeps up (the screen's next frame comes first - no lag), the same priority
 * as soon as frames start waiting for it (none gets dropped). */
#define WRITER_NICE 5

static void *writer_thread(void *arg) {
    Rec *r = arg;
    id_t tid = (id_t)syscall(SYS_gettid);
    int base = getpriority(PRIO_PROCESS, tid), niced = setpriority(PRIO_PROCESS, tid, base + WRITER_NICE) == 0;
    for (;;) {
        pthread_mutex_lock(&r->lock);
        while (!r->qn && !r->quit) pthread_cond_wait(&r->cond, &r->lock);
        if (!r->qn && r->quit) {
            pthread_mutex_unlock(&r->lock);
            break;
        }
        VItem *it = r->q[0];
        double t = r->qt[0];
        memmove(&r->q[0], &r->q[1], sizeof(void *) * (r->qn - 1));
        memmove(&r->qt[0], &r->qt[1], sizeof(double) * (r->qn - 1));
        int waiting = --r->qn;
        pthread_mutex_unlock(&r->lock);
        if (niced && (waiting >= 3 || r->quit)) {           /* falling behind (or finishing): keep up */
            if (setpriority(PRIO_PROCESS, tid, base) == 0) niced = 0;
        } else if (!niced && waiting == 0 && !r->quit) {
            niced = setpriority(PRIO_PROCESS, tid, base + WRITER_NICE) == 0;
        }
        double t0 = mono(), c0 = thread_cpu();
        if (!r->error) {
            int before = r->frames;
            encode_item(r, it);
            if (r->frames > before) r->mov.times[r->mov.n - 1] = t;     /* its real time */
        } else {
            item_free(it);
        }
        free(it);
        r->spent += mono() - t0;
        r->spent_cpu += thread_cpu() - c0;
    }
    return NULL;
}

static void queue_item(Rec *r, VItem *it, double t) {
    pthread_mutex_lock(&r->lock);
    if (r->qn < MAXQ) {
        r->q[r->qn] = it;
        r->qt[r->qn++] = t;
        if (r->qn > r->queue_max) r->queue_max = r->qn;
        it = NULL;
        pthread_cond_signal(&r->cond);
    } else {
        r->dropped++;
    }
    pthread_mutex_unlock(&r->lock);
    if (it) {
        item_free(it);
        free(it);
    }
}

static void start_writer(Rec *r, int w, int h) {
    r->vw = w;
    r->vh = h;
    jm_init(&r->jm, w, h, VIDEO_QUALITY, 2.0);
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    pthread_create(&r->th, NULL, writer_thread, r);
}

static void finish_writer(Rec *r) {
    pthread_mutex_lock(&r->lock);
    r->quit = 1;
    pthread_cond_signal(&r->cond);
    pthread_mutex_unlock(&r->lock);
    pthread_join(r->th, NULL);
    jm_close(&r->jm);
}

static void enc_release(int cookie) { cam_release_raw(cookie); }

/* a video recording into `raw` (instant: the look applied while recording) */
static Rec *rec_new_video(const char *raw, int instant) {
    Rec *r = calloc(1, sizeof *r);
    r->kind = 'v';
    r->look = A.st.filter;
    r->turn = A.cam_turn;
    snprintf(r->raw, sizeof r->raw, "%s", raw);
    sd_hold();                                       /* (recording: the card writable until it is saved) */
    if (instant) {
        int w, h;
        instant_size(r->look, &w, &h);
        if (mov_open(&r->mov, r->raw, w, h, 0, LOOK_NAMES[r->look])) goto fail;
        r->live = 1;
        start_writer(r, w, h);
    } else {
        const CamStream *m = cam_main();
        if (mov_open(&r->mov, r->raw, m->w, m->h, r->turn ? 90 : 0, LOOK_NAMES[r->look])) goto fail;
        if (!A.hw_video_failed)
            r->venc = venc_open(m->w, m->h, m->stride, m->frame_size, VIDEO_FPS, &r->mov, enc_release);
        if (r->venc) {
            r->hw = 1;
        } else {                                     /* no hardware encoder: software, unfiltered */
            if (!A.hw_video_failed) logw("Hardware video recording not available - using software");
            /* ((v53) its driver not loaded yet - just after start-up: tried again next time) */
            A.hw_video_failed = !access("/dev/video11", F_OK) || getenv("ZEROICHI_SIM") != NULL;
            r->sw = 1;
            start_writer(r, m->w, m->h);
        }
    }
    r->start = mono();
    r->cpu_t = mono();
    return r;
fail:
    loge("Could not start the video file %s", r->raw);
    sd_release();
    free(r);
    return NULL;
}

static void start_video(void) {
    char path[512], raw[512];
    new_media_path("VID", ".mov", path, sizeof path);
    snprintf(raw, sizeof raw, "%.*s.part", (int)(strlen(path) - 4), path);
    Rec *r = rec_new_video(raw, instant_video(A.st.filter));
    if (!r) {
        unreserve(path);
        show_toast("Video failed!", 2);
        return;
    }
    snprintf(r->path, sizeof r->path, "%s", path);
    A.rec = r;
}

static void start_gif(void) {
    Rec *r = calloc(1, sizeof *r);
    r->kind = 'g';
    r->look = A.st.filter;
    r->turn = A.cam_turn;
    r->target = A.st.gif_frames;
    r->gy = calloc(r->target, sizeof(Img));
    r->gu = calloc(r->target, sizeof(Img));
    r->gv = calloc(r->target, sizeof(Img));
    r->gt = calloc(r->target, sizeof(double));
    r->start = mono();
    A.rec = r;
}

void trigger(void) {
    if (A.cam_missing || !cam_configured()) {
        show_toast("No camera!", 1.5);
        return;
    }
    if (card_full()) {
        show_toast("Card full!", 2);
        return;
    }
    if (mode() == MODE_PHOTO) take_photo();
    else if (mode() == MODE_GIF) start_gif();
    else start_video();
}

/* a frame while recording (after the live view has shown it) */
/* (v54) the video's pictures: 15 a second. The camera may run at 30 (the
 * screen smoother while recording - live_rate): every other picture then, by
 * the camera's own time stamps. GIFs: every one. */
static int rec_takes(Rec *r, const CamFrame *f) {
    if (r->kind != 'v' || f->ts_us <= 0) return 1;
    const int64_t step = (int64_t)(1e6 / VIDEO_FPS);
    if (r->due_set && f->ts_us < r->due_ts - step / 4) return 0;
    r->due_ts = f->ts_us + step;
    r->due_set = 1;
    return 1;
}

/* (v54) a picture's time in the video: by the camera's own time stamp (when
 * it was taken - not when the live view got to it: smoother timing) */
static double rec_time(Rec *r, const CamFrame *f, double now) {
    if (f->ts_us <= 0) return now - r->start;
    if (!r->ts0) {
        r->ts0 = f->ts_us;
        r->t0 = now - r->start;
    }
    double t = r->t0 + (f->ts_us - r->ts0) / 1e6;
    return t > now - r->start + 1 || t < r->t0 ? now - r->start : t;   /* (an odd stamp: as before) */
}

/* the video writer's work per picture, for live_rate: this recording's, or
 * the last one's with this look */
static double wcost[NLOOKS];
double rec_writer_cost(int look) {
    Rec *r = A.rec;
    if (look < 0 || look >= NLOOKS) return 0.02;
    if (r && r->kind == 'v' && r->look == look) {
        if (r->hw) return 0.004;
        if (r->frames >= 8) return r->spent_cpu / r->frames;
    }
    if (wcost[look] > 0) return wcost[look];
    return look == LOOK_NORMAL ? 0.004 : 0.018;
}

static void rec_checks(Rec *r, double now) {
    if (A.st.video_max > 0 && now - r->start >= A.st.video_max) stop_recording();
    else if (r->mov.kb > MAX_VIDEO_KB) {             /* (a file on the card's FAT drive: 4 GB at most) */
        stop_recording();
        show_toast("Video stopped:\nfile full (3.5 GB)", 3);
    } else if (card_full()) {
        stop_recording();
        show_toast("Card full!", 2);
    }
}

void rec_frame(const CamFrame *f, double now, void *item_in) {
    Rec *r = A.rec;
    LiveItem *li = item_in;
    if (!r) {
        release_frame(f->cookie);
        return;
    }
    if (r->kind == 'g') {
        const CamPlanes *m = &f->main;
        cam_begin_read(m);
        int k = r->n;
        r->gy[k] = img_copy(img_view((uint8_t *)m->y, m->w, m->h, 1, m->stride));
        r->gu[k] = img_copy(img_view((uint8_t *)m->u, m->w / 2, m->h / 2, 1, m->stride / 2));
        r->gv[k] = img_copy(img_view((uint8_t *)m->v, m->w / 2, m->h / 2, 1, m->stride / 2));
        cam_end_read(m);
        r->gt[k] = now;
        r->n++;
        release_frame(f->cookie);
        if (li) live_item_free(li);
        if (r->n >= r->target) stop_recording();
        return;
    }
    r->shown++;
    if (!rec_takes(r, f)) {                          /* (on the screen only - see rec_takes) */
        release_frame(f->cookie);
        if (li) live_item_free(li);
        rec_checks(r, now);
        return;
    }
    if (r->live) {
        if (li && li->kind) {
            VItem *it = calloc(1, sizeof *it);
            it->kind = li->kind;
            it->tone = li->tone;
            it->rgb = li->rgb;
            it->y = li->y; it->u = li->u; it->v = li->v;
            queue_item(r, it, rec_time(r, f, now));
            li->kind = 0;
        }
        release_frame(f->cookie);
    } else if (r->hw) {
        if (venc_put(r->venc, f->main.fd, f->cookie, f->ts_us)) release_frame(f->cookie);
        if (li) live_item_free(li);
    } else {                                         /* software: the frame as it is */
        const CamPlanes *m = &f->main;
        VItem *it = calloc(1, sizeof *it);
        it->kind = 3;
        cam_begin_read(m);
        it->y = img_copy(img_view((uint8_t *)m->y, m->w, m->h, 1, m->stride));
        it->u = img_copy(img_view((uint8_t *)m->u, m->w / 2, m->h / 2, 1, m->stride / 2));
        it->v = img_copy(img_view((uint8_t *)m->v, m->w / 2, m->h / 2, 1, m->stride / 2));
        cam_end_read(m);
        release_frame(f->cookie);
        queue_item(r, it, rec_time(r, f, now));
        if (li) live_item_free(li);
    }
    rec_checks(r, now);
}

/* the live view's own work for a recorded frame (the log's "while recording") */
void rec_note_frame_cpu(double seconds) {
    if (!A.rec) return;
    A.rec->main_cpu += seconds;
    A.rec->main_n++;
}

/* frames for the hardware encoder that the live view skipped */
void rec_extra_frame(const CamFrame *f) {
    Rec *r = A.rec;
    if (r && r->hw && rec_takes(r, f) && venc_put(r->venc, f->main.fd, f->cookie, f->ts_us) == 0) return;
    release_frame(f->cookie);
}

/* ------------------------------------------------------------- GIF saving */
typedef struct {
    Img *y, *u, *v;
    double *t;
    int n, look, turn;
    char path[512];
} GifJob;

static void gif_wait_for_video(void) {
    while (A.rec && A.rec->kind == 'v') msleep(200);       /* a video goes first */
}

static void save_gif(void *arg) {
    GifJob *j = arg;
    double t0 = mono();
    sd_hold();
    int n = j->n;
    double gaps = 0;
    for (int i = 1; i < n; i++) gaps += j->t[i] - j->t[i - 1];
    int ms = (int)(round(1000 * gaps / (n - 1) / 10) * 10);
    if (ms < 60) ms = 60;
    Img *idx = calloc(n, sizeof(Img));
    uint8_t pal[256][3];
    int ncol = 0;
    if (look_is_pixel(j->look) || look_is_tone(j->look)) {
        const Palette *p = NULL;
        int used[256] = {0}, fw[n], fh[n];
        for (int i = 0; i < n; i++) {
            gif_wait_for_video();
            Img Y = j->y[i], U = j->u[i], V = j->v[i];
            if (j->turn) {
                Y = img_rot90cw(j->y[i]);
                U = img_rot90cw(j->u[i]);
                V = img_rot90cw(j->v[i]);
            }
            Tone tn = look_is_pixel(j->look) ? pixel_tone_planes(j->look, Y, U, V, 0, 0)
                                             : tone_make(j->look, Y, 0, 0);
            /* (the colour numbers at the tone's own size - small for the Game
             *  Boy and 16-Bit looks - enlarged once they are final, below) */
            p = tone_palette(&tn);
            idx[i] = img_copy(tn.t);
            img_point(idx[i], p->gif_index);
            fw[i] = tn.w;
            fh[i] = tn.h;
            tone_free(&tn);
            if (j->turn) { img_free(&Y); img_free(&U); img_free(&V); }
            for (int yy = 0; yy < idx[i].h; yy++) {
                const uint8_t *row = PX(idx[i], 0, yy);
                for (int x = 0; x < idx[i].w; x++) used[row[x]] = 1;
            }
        }
        /* only the colours used, numbered from 0 */
        uint8_t map[256];
        for (int c = 0; c < 256; c++)
            if (used[c]) {
                map[c] = (uint8_t)ncol;
                pal[ncol][0] = p->r[c];
                pal[ncol][1] = p->g[c];
                pal[ncol][2] = p->b[c];
                ncol++;
            }
        for (int i = 0; i < n; i++) {
            img_point(idx[i], map);
            if (idx[i].w != fw[i] || idx[i].h != fh[i]) {          /* sharp pixel edges */
                Img big = img_resize(idx[i], fw[i], fh[i], RS_NEAREST);
                img_free(&idx[i]);
                idx[i] = big;
            }
        }
    } else {
        Img *rgb = calloc(n, sizeof(Img));
        for (int i = 0; i < n; i++) {
            gif_wait_for_video();
            Img Y = j->y[i], U = j->u[i], V = j->v[i];
            Img uu = img_resize(U, Y.w, Y.h, RS_NEAREST), vv = img_resize(V, Y.w, Y.h, RS_NEAREST);
            Img c = img_ycc_to_rgb(Y, uu, vv);
            img_free(&uu); img_free(&vv);
            if (j->turn) {
                Img t = img_rot90cw(c);
                img_free(&c);
                c = t;
            }
            rgb[i] = look_rgb(j->look, c, 0, 0);
            img_free(&c);
        }
        ncol = gif_quantize(rgb, n, 128, pal, idx);
        for (int i = 0; i < n; i++) img_free(&rgb[i]);
        free(rgb);
    }
    int w = idx[0].w, h = idx[0].h;
    int bad = gif_write(j->path, idx, n, (const uint8_t(*)[3])pal, ncol < 1 ? 1 : ncol, ms, LOOK_NAMES[j->look]);
    sync_all();
    const char *name = strrchr(j->path, '/') + 1;
    if (bad) {
        loge("Saving %s failed", name);
        unlink(j->path);
        add_result("Save failed!", 2, 0, NULL, 0, 0, 0, j->path);
    } else {
        logi("Saved %s (%d frames %dx%d, %d ms, %d colours) in %.1f s", name, n, w, h, ms, ncol, mono() - t0);
        add_result(NULL, 0, 1, "gif", j->look, (double)w * h * n, (double)file_size(j->path), j->path);
    }
    sd_release();
    for (int i = 0; i < n; i++) {
        img_free(&idx[i]);
        img_free(&j->y[i]);
        img_free(&j->u[i]);
        img_free(&j->v[i]);
    }
    free(idx);
    free(j->y); free(j->u); free(j->v); free(j->t);
    free(j);
}

static void finish_gif(Rec *r) {
    if (r->n < 2) {
        for (int i = 0; i < r->n; i++) { img_free(&r->gy[i]); img_free(&r->gu[i]); img_free(&r->gv[i]); }
        free(r->gy); free(r->gu); free(r->gv); free(r->gt);
        free(r);
        show_toast("GIF cancelled", 1.5);
        return;
    }
    GifJob *j = calloc(1, sizeof *j);
    j->y = r->gy; j->u = r->gu; j->v = r->gv; j->t = r->gt;
    j->n = r->n;
    j->look = r->look;
    j->turn = r->turn;
    logi("GIF %dx%d: %d frames in %.1f s", r->turn ? r->gy[0].h : r->gy[0].w, r->turn ? r->gy[0].w : r->gy[0].h,
         r->n, r->gt[r->n - 1] - r->gt[0]);
    new_media_path("GIF", ".gif", j->path, sizeof j->path);
    free(r);
    worker_submit(&A.saver, save_gif, j);
    show_toast("Saving GIF...", 1.0);
    drain_events();
}

/* ------------------------------------------------------------- videos */
static int missed_frames(const MovWriter *m) {
    int missed = 0;
    for (int i = 1; i < m->n; i++) {
        long k = lround((m->times[i] - m->times[i - 1]) * VIDEO_FPS) - 1;
        if (k > 0) missed += (int)k;
    }
    return missed;
}

static void finish_video(Rec *r) {
    double seconds = fmax(0.1, mono() - r->start);
    char cpu[256], health[96];
    cpu_report(cpu, sizeof cpu, 1);
    if (r->venc) venc_close(r->venc);
    if (r->live || r->sw) {
        if (r->qn > 3) show_message("Saving...", "one moment", (Rgb){20, 20, 28});
        finish_writer(r);
    }
    int n = r->mov.n;
    double dur = mov_duration(&r->mov);
    int missed = missed_frames(&r->mov);
    int w = r->mov.w, h = r->mov.h;
    double times_last = n ? r->mov.times[n - 1] : 0;
    (void)times_last;
    mov_close(&r->mov);
    sync_all();
    const char *name = strrchr(r->path, '/') + 1;
    if (n < 2 || r->error) {
        unlink(r->raw);
        unreserve(r->path);
        show_toast(r->error ? "Video failed!" : "Video too short", 2);
        sd_release();
        free(r);
        return;
    }
    logi("Recorded %s: %d frames in %.1f s = %.1f frames/s (%s, %s, %dx%d), %d frames missed, %d dropped by the "
         "writer; screen showed %.1f frames/s",
         name, n, dur, dur > 0 ? (n - 1) / dur : 0, r->live ? "instant" : r->hw ? "hardware encoder" : "software",
         LOOK_NAMES[r->look], w, h, missed, r->dropped, r->shown / seconds);
    logi("CPU while recording: %s; Pi: %s", cpu, pi_health(health, sizeof health));
    if (r->live && r->frames >= 8 && r->look >= 0 && r->look < NLOOKS) wcost[r->look] = r->spent_cpu / r->frames;
    if (r->live && r->frames)
        logi("While recording: live view %.0f ms per frame; video writer %.0f ms per frame (%.0f ms working), "
             "most frames waiting %d", r->main_n ? 1000 * r->main_cpu / r->main_n : 0.0,
             1000 * r->spent / r->frames, 1000 * r->spent_cpu / r->frames, r->queue_max);
    if (r->live || r->look == LOOK_NORMAL) {         /* nothing to develop: ready now */
        learn_space("video", r->look, (double)w * h * n, (double)file_size(r->raw));
        rename(r->raw, r->path);
        sync_all();
        unreserve(r->path);
        A.count++;
        char t[64];
        snprintf(t, sizeof t, "Saved %.0f s video", dur);
        show_toast(t, 1.5);
    } else {                                         /* the look is applied afterwards */
        char note[600], js[700];
        snprintf(note, sizeof note, "%s.json", r->raw);
        snprintf(js, sizeof js, "{\"final\": \"%s\", \"filter\": \"%s\"}", name, LOOK_NAMES[r->look]);
        write_file(note, js, strlen(js), 1);
        sync_all();
        develop_submit(r->raw);
        enter_developing();
    }
    sd_release();
    free(r);
}

void stop_recording(void) {
    Rec *r = A.rec;
    if (!r) return;
    A.rec = NULL;
    if (r->kind == 'g') finish_gif(r);
    else finish_video(r);
    drain_events();
}

/* ------------------------------------------------------------- developing */
typedef struct { char raw[512]; } DevJob;

/* (preview: a half-size picture of the frame - shown on "Developing film...") */
static void dev_set(double p, Img *preview) {
    A.dev_progress = p;
    if (preview && preview->p) {
        pthread_mutex_lock(&A.dev_lock);
        if (A.dev_preview.p) img_free(&A.dev_preview);
        A.dev_preview = *preview;
        preview->p = NULL;
        pthread_mutex_unlock(&A.dev_lock);
    }
}

static int note_value(const char *js, const char *key, char *out, size_t cap) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\"", key);
    const char *p = strstr(js, k);
    if (!p) return -1;
    p = strchr(p + strlen(k), '"');
    if (!p) return -1;
    p++;
    size_t n = 0;
    while (*p && *p != '"' && n + 1 < cap) out[n++] = *p++;
    out[n] = 0;
    return 0;
}

/* developing, frame by frame: decode, apply the look (dev_prepare), compress
 * (dev_encode) - a port of FrameDeveloper.
 * develop() runs the two side by side: while the video encoder (the Pi's
 * graphics chip) compresses one frame, the next one is decoded and filtered. */
typedef struct {
    MovReader rd;
    int look, turn, w, h;           /* the developed video's size (turned) */
    JpegMaker jm;
    double spent[3];                /* decode, filter, compress */
} Dev;

typedef struct {
    int yuv;                        /* 1: Y, U, V planes - 0: an R,G,B picture */
    Img Y, U, V, rgb;
} DevFrame;

static void dev_frame_free(DevFrame *f) {
    img_free(&f->Y); img_free(&f->U); img_free(&f->V); img_free(&f->rgb);
}

static int dev_open(Dev *d, const char *raw, int look) {
    memset(d, 0, sizeof *d);
    if (movr_open(&d->rd, raw)) return -1;
    d->look = look;
    d->turn = d->rd.rotation == 90;
    d->w = d->turn ? d->rd.h : d->rd.w;
    d->h = d->turn ? d->rd.w : d->rd.h;
    jm_init(&d->jm, d->w, d->h, VIDEO_QUALITY, 2.0);
    return 0;
}

static void dev_close(Dev *d) {
    movr_close(&d->rd);
    jm_close(&d->jm);
}

/* a tone's picture at half size (the preview) */
static Img tone_preview(Tone *tn) {
    Tone t = *tn;
    t.w = tn->w / 2 > 0 ? tn->w / 2 : 1;
    t.h = tn->h / 2 > 0 ? tn->h / 2 : 1;
    return tone_rgb(&t);
}

/* frame i: decoded and filtered, ready to compress; preview: a half-size
 * picture of it (if not NULL) */
static int dev_prepare(Dev *d, int i, DevFrame *f, Img *preview) {
    int look = d->look, w = d->w, h = d->h, turn = d->turn;
    memset(f, 0, sizeof *f);
    double t0 = mono();
    size_t len;
    uint8_t *data = movr_frame(&d->rd, i, &len);
    if (!data) return -1;
    int sw = d->rd.w, sh = d->rd.h;
    if (look_is_pixel(look)) {                       /* 16-Bit: decoded small, as YUV */
        int gw, gh;
        pixel_grid(sw, sh, &gw, &gh);
        Img y, cb, cr;
        if (jpeg_decode_ycc(data, len, gw, gh, &y, &cb, &cr)) {
            free(data);
            return -1;
        }
        if (turn) {
            Img a = img_rot90cw(y), b = img_rot90cw(cb), c = img_rot90cw(cr);
            img_free(&y); img_free(&cb); img_free(&cr);
            y = a; cb = b; cr = c;
        }
        double t1 = mono();
        Tone tn = pixel_tone_planes(look, y, cb, cr, w, h);
        img_free(&y); img_free(&cb); img_free(&cr);
        f->yuv = 1;
        tone_yuv(&tn, 0, 0, 0, &f->Y, &f->U, &f->V);
        if (preview) *preview = tone_preview(&tn);
        tone_free(&tn);
        d->spent[0] += t1 - t0; d->spent[1] += mono() - t1;
    } else {
        int half = look_half_ok(look);
        Img a = jpeg_decode(data, len, half ? sw / 2 : 0, half ? sh / 2 : 0, look_is_tone(look));
        if (!a.p) {
            free(data);
            return -1;
        }
        if (turn) {
            Img r = img_rot90cw(a);
            img_free(&a);
            a = r;
        }
        double t1 = mono();
        if (look_is_tone(look)) {
            Tone tn = tone_make(look, a, w, h);
            f->yuv = 1;
            tone_yuv(&tn, 0, 0, 0, &f->Y, &f->U, &f->V);
            if (preview) *preview = tone_preview(&tn);
            tone_free(&tn);
        } else {
            Img o = look_rgb(look, a, 0, 0);
            if (o.w != w || o.h != h) {              /* enlarge once, back to full size */
                Img r = img_resize(o, w, h, RS_NEAREST);
                img_free(&o);
                o = r;
            }
            f->rgb = o;
            if (preview) *preview = img_reduce(o, 2);
        }
        img_free(&a);
        d->spent[0] += t1 - t0; d->spent[1] += mono() - t1;
    }
    free(data);
    return 0;
}

/* a prepared frame -> JPEG (out) */
static int dev_encode(Dev *d, DevFrame *f, Buf *out) {
    double t0 = mono();
    int bad = f->yuv ? jm_yuv(&d->jm, f->Y.p, f->Y.stride, f->U.p, f->V.p, f->U.stride, out)
                     : jm_rgb(&d->jm, f->rgb, out);
    d->spent[2] += mono() - t0;
    return bad ? -1 : 0;
}

/* the compressing side of develop(): takes each prepared frame in turn,
 * compresses it and adds it to the video */
typedef struct {
    Dev *d;
    MovWriter *wr;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    DevFrame slot;
    double dur;                     /* the frame's duration */
    int full, done, bad;
} DevPipe;

static void *dev_writer(void *arg) {
    DevPipe *pp = arg;
    double t = 0;
    for (;;) {
        pthread_mutex_lock(&pp->lock);
        while (!pp->full && !pp->done) pthread_cond_wait(&pp->cond, &pp->lock);
        if (!pp->full) {                             /* (done, nothing left) */
            pthread_mutex_unlock(&pp->lock);
            break;
        }
        DevFrame f = pp->slot;
        double dur = pp->dur;
        pthread_mutex_unlock(&pp->lock);
        Buf out = {0};
        int bad = pp->bad || dev_encode(pp->d, &f, &out) || mov_add(pp->wr, out.p, out.n, t);
        buf_free(&out);
        dev_frame_free(&f);
        t += dur;
        pthread_mutex_lock(&pp->lock);
        if (bad) pp->bad = 1;
        pp->full = 0;
        pthread_cond_broadcast(&pp->cond);
        pthread_mutex_unlock(&pp->lock);
    }
    return NULL;
}

/* develop_video: the whole video, into the final file */
static int develop(const char *raw, char *final_out, size_t cap, int *look_out, double *pixels, double *bytes) {
    char note[600];
    snprintf(note, sizeof note, "%s.json", raw);
    char *js = read_file(note, NULL);
    if (!js) return -1;
    char fname[256] = "", look_name[64] = "Normal", pal[32] = "";
    note_value(js, "final", fname, sizeof fname);
    note_value(js, "filter", look_name, sizeof look_name);
    note_value(js, "palette", pal, sizeof pal);
    free(js);
    int look = look_by_name(look_name);
    if ((!strcmp(look_name, "16-Bit") || !strcmp(look_name, "8-Bit")) && pal[0]) {
        char n2[64];
        snprintf(n2, sizeof n2, "16-Bit %s", pal);
        if (look_by_name(n2) >= 0) look = look_by_name(n2);
    }
    if (look < 0) look = LOOK_NORMAL;
    *look_out = look;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", raw);
    *strrchr(dir, '/') = 0;
    snprintf(final_out, cap, "%s/%s", dir, fname);
    double t_start = mono();
    Dev d;
    if (dev_open(&d, raw, look)) return -1;
    int n = d.rd.n;
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s.dev", final_out);
    MovWriter wr;
    if (mov_open(&wr, tmp, d.w, d.h, 0, LOOK_NAMES[look])) {
        dev_close(&d);
        return -1;
    }
    DevPipe pp;
    memset(&pp, 0, sizeof pp);
    pp.d = &d;
    pp.wr = &wr;
    pthread_mutex_init(&pp.lock, NULL);
    pthread_cond_init(&pp.cond, NULL);
    pthread_t wt;
    int started = pthread_create(&wt, NULL, dev_writer, &pp) == 0, bad = !started;
    for (int i = 0; i < n && !bad; i++) {
        while (A.rec) msleep(200);                   /* recording: it gets the processor */
        int want_preview = i % 5 == 0;
        Img preview = {0};
        DevFrame f;
        bad = dev_prepare(&d, i, &f, want_preview ? &preview : NULL);
        pthread_mutex_lock(&pp.lock);
        while (pp.full) pthread_cond_wait(&pp.cond, &pp.lock);   /* (the one before: taken) */
        if (pp.bad) bad = 1;
        if (!bad) {
            pp.slot = f;
            pp.dur = d.rd.durations[i];
            pp.full = 1;
            pthread_cond_broadcast(&pp.cond);
        }
        pthread_mutex_unlock(&pp.lock);
        if (bad) dev_frame_free(&f);
        dev_set((double)(i + 1) / n, want_preview && !bad ? &preview : NULL);
        if (preview.p) img_free(&preview);
    }
    if (started) {                                   /* the last frame, then the writer ends */
        pthread_mutex_lock(&pp.lock);
        pp.done = 1;
        pthread_cond_broadcast(&pp.cond);
        pthread_mutex_unlock(&pp.lock);
        pthread_join(wt, NULL);
        if (pp.bad) bad = 1;
    }
    pthread_mutex_destroy(&pp.lock);
    pthread_cond_destroy(&pp.cond);
    mov_close(&wr);
    int w = d.w, h = d.h;
    char how[64];
    snprintf(how, sizeof how, "%s", jm_how(&d.jm));
    double sp[3] = {d.spent[0], d.spent[1], d.spent[2]};
    dev_close(&d);
    if (bad) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, final_out)) return -1;
    unlink(raw);
    unlink(note);
    sync_all();
    logi("Developed %s (%s, %d frames, %dx%d) in %.0f s; per frame: decode %.0f ms, filter %.0f ms, "
         "compress %.0f ms (%s)", strrchr(final_out, '/') + 1, LOOK_NAMES[look], n, w, h, mono() - t_start,
         1000 * sp[0] / (n ? n : 1), 1000 * sp[1] / (n ? n : 1), 1000 * sp[2] / (n ? n : 1), how);
    *pixels = (double)w * h * n;
    *bytes = (double)file_size(final_out);
    return 0;
}

static void develop_job(void *arg) {
    DevJob *j = arg;
    char final[600];
    int look = 0;
    double px = 0, by = 0;
    A.dev_progress = 0;
    sd_hold();                                       /* (developing = saving: the card writable) */
    if (develop(j->raw, final, sizeof final, &look, &px, &by) == 0) {
        add_result("Video developed", 1.5, 1, "video", look, px, by, final);
    } else {
        /* never lose a video: after a second failed try keep it unfiltered */
        char note[600];
        snprintf(note, sizeof note, "%s.json", j->raw);
        char *js = read_file(note, NULL);
        int failures = 0;
        char fname[256] = "";
        if (js) {
            const char *p = strstr(js, "\"failures\"");
            if (p) failures = atoi(strchr(p, ':') + 1);
            note_value(js, "final", fname, sizeof fname);
        }
        failures++;
        loge("Developing %s failed (%d)", strrchr(j->raw, '/') + 1, failures);
        if (failures < 2 && js) {
            char *end = strrchr(js, '}');
            if (end) {
                char *p = strstr(js, ", \"failures\"");
                if (p) end = p;
                *end = 0;
                char js2[1024];
                snprintf(js2, sizeof js2, "%s, \"failures\": %d}", js, failures);
                write_file(note, js2, strlen(js2), 1);
            }
            add_result("Developing failed!", 2, 0, NULL, 0, 0, 0, NULL);
        } else if (fname[0]) {
            char dir[512], dst[800];
            snprintf(dir, sizeof dir, "%s", j->raw);
            *strrchr(dir, '/') = 0;
            snprintf(dst, sizeof dst, "%s/%s", dir, fname);
            rename(j->raw, dst);
            unlink(note);
            sync_all();
            logw("Kept %s without the filter (developing failed twice)", fname);
            add_result("Video saved (no filter)", 1.5, 1, NULL, 0, 0, 0, dst);
        }
        free(js);
    }
    sd_release();
    A.dev_progress = -1;
    free(j);
}

void develop_submit(const char *raw) {
    DevJob *j = calloc(1, sizeof *j);
    snprintf(j->raw, sizeof j->raw, "%s", raw);
    worker_submit(&A.developer, develop_job, j);
}

void resume_developing(void) {
    DIR *d = opendir(A.photo_dir);
    if (!d) return;
    struct dirent *e;
    char **names = NULL;
    int n = 0;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        size_t l = strlen(nm);
        char p[800];
        snprintf(p, sizeof p, "%s/%s", A.photo_dir, nm);
        if (!strncmp(nm, "VID_", 4) && l > 8 && !strcmp(nm + l - 8, ".mov.dev")) {
            sd_unlink(p);                            /* half-written results */
        } else if (!strncmp(nm, "VID_", 4) && l > 5 && !strcmp(nm + l - 5, ".part")) {
            names = realloc(names, sizeof(char *) * (n + 1));
            names[n++] = strdup(p);
        }
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char note[900];
        snprintf(note, sizeof note, "%s.json", names[i]);
        if (file_exists(note)) {
            logi("Resuming developing %s", strrchr(names[i], '/') + 1);
            develop_submit(names[i]);
        } else {
            sd_unlink(names[i]);
        }
        free(names[i]);
    }
    free(names);
}
