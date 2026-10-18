#define _GNU_SOURCE
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

double mono(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

double uptime(void) {
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

double thread_cpu(void) {
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

void msleep(int ms) {
    if (ms <= 0) return;
    struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
}

void sleep_s(double s) {
    if (s <= 0) return;
    struct timespec t = {(time_t)s, (long)((s - (time_t)s) * 1e9)};
    while (nanosleep(&t, &t) == -1 && errno == EINTR) {}
}

char *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    size_t cap = 4096, n = 0;
    char *p = malloc(cap + 1);
    for (;;) {
        if (n == cap) {
            cap *= 2;
            p = realloc(p, cap + 1);
        }
        ssize_t r = read(fd, p + n, cap - n);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) break;
        n += (size_t)r;
    }
    close(fd);
    p[n] = 0;
    if (len) *len = n;
    return p;
}

static int write_file_(const char *path, const void *data, size_t len, int fsync_it) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return -1;
    const char *p = data;
    while (len) {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) {
            close(fd);
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    if (fsync_it) fsync(fd);
    return close(fd);
}

/* (on the SD card: it is made writable for the moment - see sd_hold) */
int write_file(const char *path, const void *data, size_t len, int fsync_it) {
    int sd = sd_path(path);
    if (sd) sd_hold();
    int r = write_file_(path, data, len, fsync_it);
    if (sd) sd_release();
    return r;
}

int write_file_atomic(const char *path, const void *data, size_t len) {
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int sd = sd_path(path);
    if (sd) sd_hold();
    int r = write_file_(tmp, data, len, 1) ? -1 : rename(tmp, path);
    if (sd) sd_release();
    return r;
}

int sd_unlink(const char *path) {
    int sd = sd_path(path);
    if (sd) sd_hold();
    int r = unlink(path);
    if (sd) sd_release();
    return r;
}

int sd_rename(const char *a, const char *b) {
    int sd = sd_path(a) || sd_path(b);
    if (sd) sd_hold();
    int r = rename(a, b);
    if (sd) sd_release();
    return r;
}

int sd_rmdir(const char *path) {
    int sd = sd_path(path);
    if (sd) sd_hold();
    int r = rmdir(path);
    if (sd) sd_release();
    return r;
}

int file_exists(const char *path) { return access(path, F_OK) == 0; }

long long file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (long long)st.st_size;
}

double file_mtime(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : st.st_mtime;
}

int mkdir_p(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) return 0;     /* (there already) */
    int sd = sd_path(path);
    if (sd) sd_hold();
    char p[1024];
    snprintf(p, sizeof p, "%s", path);
    for (char *s = p + 1; *s; s++) {
        if (*s == '/') {
            *s = 0;
            mkdir(p, 0755);
            *s = '/';
        }
    }
    int r = mkdir(p, 0755) && errno != EEXIST ? -1 : 0;
    if (sd) sd_release();
    return r;
}

int is_mount(const char *path) {
    struct stat a, b;
    char parent[1024];
    if (stat(path, &a)) return 0;
    snprintf(parent, sizeof parent, "%s/..", path);
    if (stat(parent, &b)) return 0;
    return a.st_dev != b.st_dev || a.st_ino == b.st_ino;
}

long long free_bytes(const char *path) {
    struct statvfs s;
    return statvfs(path, &s) ? -1 : (long long)s.f_bavail * (long long)s.f_frsize;
}

long long total_bytes(const char *path) {
    struct statvfs s;
    return statvfs(path, &s) ? -1 : (long long)s.f_blocks * (long long)s.f_frsize;
}

int copy_file(const char *src, const char *dst) {
    size_t n;
    char *d = read_file(src, &n);
    if (!d) return -1;
    int r = write_file(dst, d, n, 1);
    free(d);
    return r;
}

void sync_all(void) { sync(); }

/* The system partition is read-only while Zero Ichi runs (the fast start sets it
 * so): nothing of yours is kept there, and pulling the plug can't harm it.
 * The few files the fast start itself needs (the start-up picture's screen
 * settings, a plan for "Photo space" or a card image) are written in a short
 * window. Nested calls are fine. (Under the normal start-up it is writable
 * anyway: then this does nothing.) */
static int sys_ro = -1, sys_depth;
static pthread_mutex_t sys_lock = PTHREAD_MUTEX_INITIALIZER;   /* (also used from worker threads) */

static void sys_write_locked(int on);
void sys_write(int on) {
    pthread_mutex_lock(&sys_lock);
    sys_write_locked(on);
    pthread_mutex_unlock(&sys_lock);
}

static void sys_write_locked(int on) {
    if (sys_ro < 0) {
        struct statvfs v;
        sys_ro = !getenv("ZEROICHI_SIM") && geteuid() == 0 && statvfs("/", &v) == 0 && (v.f_flag & ST_RDONLY);
    }
    if (!sys_ro) return;
    if (on) {
        if (sys_depth++ == 0 && mount(NULL, "/", NULL, MS_REMOUNT, NULL))
            logw("System partition: could not open it for writing (%s)", strerror(errno));
    } else if (sys_depth > 0 && --sys_depth == 0) {
        sync();
        int i;
        for (i = 0; i < 20 && mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL); i++) msleep(100);
        if (i == 20) logw("System partition: still writable (%s)", strerror(errno));
    }
}

int run_cmd(const char *cmd) {
    int r = system(cmd);
    return r < 0 ? -1 : (r >> 8) & 255;
}

/* ------------------------------------------------------------------ log */
/* zeroichi-log.txt. When the SD card is read-only between writes (sd_init),
 * new lines are kept in memory and added to the file whenever the card is
 * writable anyway, or once a minute (errors: within seconds). */
static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_f;
static char *log_mem;
static size_t log_n, log_cap;
static char log_path[600];
static int log_buffered, log_appending, log_urgent;
static double log_written;
static int sd_on;

static void log_line(const char *level, const char *fmt, va_list ap) {
    char msg[2048], line[2200];
    vsnprintf(msg, sizeof msg, fmt, ap);
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    localtime_r(&tv.tv_sec, &tm);
    int n = snprintf(line, sizeof line, "%04d-%02d-%02d %02d:%02d:%02d,%03d %s %s\n",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
                     tm.tm_sec, (int)(tv.tv_usec / 1000), level, msg);
    if (n < 0) return;
    if (n > (int)sizeof line - 1) n = sizeof line - 1;
    pthread_mutex_lock(&log_lock);
    fputs(line, stderr);
    if (log_f && !log_buffered) {
        fputs(line, log_f);
        fflush(log_f);
    } else if (log_n < 2000000) {
        if (log_n + n + 1 > log_cap) {
            log_cap = (log_n + n + 1) * 2;
            log_mem = realloc(log_mem, log_cap);
        }
        memcpy(log_mem + log_n, line, n);
        log_n += n;
        if (level[0] == 'E') log_urgent = 1;
    }
    pthread_mutex_unlock(&log_lock);
}

/* (buffered: the lines kept so far onto the card - it must be writable) */
static void log_write_kept(void) {
    pthread_mutex_lock(&log_lock);
    if (log_buffered && log_path[0] && log_n) {
        FILE *f = fopen(log_path, log_appending ? "a" : "w");
        if (f) {
            fwrite(log_mem, 1, log_n, f);
            fflush(f);
            fsync(fileno(f));
            fclose(f);
            log_appending = 1;
            log_n = 0;
        }
    }
    log_urgent = 0;
    log_written = mono();
    pthread_mutex_unlock(&log_lock);
}

static int log_kept(void) {
    pthread_mutex_lock(&log_lock);
    int n = log_buffered && log_path[0] && log_n > 0;
    pthread_mutex_unlock(&log_lock);
    return n;
}

void log_open(const char *path, int append) {
    if (sd_on) {                                    /* buffered: written with the card's other writes */
        pthread_mutex_lock(&log_lock);
        snprintf(log_path, sizeof log_path, "%s", path);
        log_buffered = 1;
        log_appending = append;
        pthread_mutex_unlock(&log_lock);
        sd_hold();
        log_write_kept();
        sd_release();
        return;
    }
    FILE *f = fopen(path, append ? "a" : "w");
    pthread_mutex_lock(&log_lock);
    if (f) {
        if (log_n) fwrite(log_mem, 1, log_n, f);
        fflush(f);
        if (log_f) fclose(log_f);
        log_f = f;
        free(log_mem);
        log_mem = NULL;
        log_n = log_cap = 0;
    }
    pthread_mutex_unlock(&log_lock);
}

void log_flush(void) {
    if (log_kept()) {
        sd_hold();
        log_write_kept();
        sd_release();
        return;
    }
    pthread_mutex_lock(&log_lock);
    if (log_f) {
        fflush(log_f);
        fsync(fileno(log_f));
    }
    pthread_mutex_unlock(&log_lock);
}

/* ------------------------------------------------------- SD card: read-only */
/* Pulling the plug while a drive is being written can damage it - so with the
 * fast start Zero Ichi keeps the SD card's drives (ZEROICHI, PHOTOS) read-only
 * whenever nothing is being saved: they are made writable only while a photo,
 * GIF or video, the settings or the log are written (sd_hold ... sd_release),
 * and read-only again 2 s after the last write. Read-only, a drive is always
 * complete and closed properly: unplugging can't harm it. */
static pthread_mutex_t sd_lock = PTHREAD_MUTEX_INITIALIZER;
static char sd_mp[2][256];
static int sd_n, sd_count, sd_rw[2] = {1, 1};
static int sd_lines_kb = 64;       /* (the log: kept in memory up to this much)*/
static double sd_since, sd_retry;
static double sd_not_before;
static int sd_quit;
static pthread_t sd_th;
static pthread_cond_t sd_cond = PTHREAD_COND_INITIALIZER;
static void *sd_thread(void *arg);

static int sd_remount(int i, int ro) {
    unsigned long fl = MS_REMOUNT | (ro ? MS_RDONLY : 0);
    struct statvfs v;
    if (statvfs(sd_mp[i], &v) == 0) {              /* (keep the mount's other settings) */
        if (v.f_flag & ST_NOSUID) fl |= MS_NOSUID;
        if (v.f_flag & ST_NODEV) fl |= MS_NODEV;
        if (v.f_flag & ST_NOEXEC) fl |= MS_NOEXEC;
        if (v.f_flag & ST_NOATIME) fl |= MS_NOATIME;
        if (v.f_flag & ST_NODIRATIME) fl |= MS_NODIRATIME;
        if (v.f_flag & ST_RELATIME) fl |= MS_RELATIME;
    }
    return mount(NULL, sd_mp[i], NULL, fl, NULL);
}

/* (again later for another folder: its drive is added) */
void sd_init(const char *dir1, const char *dir2) {
    if (geteuid() != 0 || !getenv("ZEROICHI_FASTINIT") || getenv("ZEROICHI_SD_ALWAYS_RW")) return;
    const char *types = getenv("ZEROICHI_SD_FSTYPES");            /* (tests) */
    if (!types) types = "vfat";
    char *m = read_file("/proc/mounts", NULL);
    if (!m) return;
    const char *dirs[2] = {dir1, dir2};
    pthread_mutex_lock(&sd_lock);
    for (int d = 0; d < 2; d++) {
        if (!dirs[d] || !dirs[d][0]) continue;
        char best[256] = "", btype[32] = "";
        char *save = NULL, *copy = strdup(m);
        for (char *ln = strtok_r(copy, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save)) {
            char dev[256], mp[256], ty[32];
            if (sscanf(ln, "%255s %255s %31s", dev, mp, ty) != 3) continue;
            size_t l = strlen(mp);
            int under = !strcmp(mp, "/") || (!strncmp(dirs[d], mp, l) && (dirs[d][l] == '/' || !dirs[d][l]));
            if (under && l >= strlen(best)) {
                snprintf(best, sizeof best, "%s", mp);
                snprintf(btype, sizeof btype, "%s", ty);
            }
        }
        free(copy);
        char want[64];
        snprintf(want, sizeof want, ",%s,", types);
        char have[40];
        snprintf(have, sizeof have, ",%s,", btype);
        if (!best[0] || !strstr(want, have) || !strcmp(best, "/")) continue;
        int dup = 0;
        for (int i = 0; i < sd_n; i++) dup |= !strcmp(sd_mp[i], best);
        if (!dup && sd_n < 2) {
            snprintf(sd_mp[sd_n], sizeof sd_mp[0], "%s", best);
            sd_rw[sd_n++] = 1;                     /* (made read-only once nothing is written) */
        }
    }
    free(m);
    if (sd_n && !sd_on) {
        sd_since = mono();
        sd_not_before = sd_since + 20;             /* (sd_started: sooner) */
        sd_on = 1;
        sd_quit = 0;
        pthread_create(&sd_th, NULL, sd_thread, NULL);
    }
    pthread_mutex_unlock(&sd_lock);
}

int sd_active(void) { return sd_on; }

/* (for the log: the drives, e.g. "/boot/firmware, /media/photos") */
void sd_drives(char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < sd_n; i++) {
        size_t l = strlen(out);
        snprintf(out + l, cap - l, "%s%s", i ? ", " : "", sd_mp[i]);
    }
}

int sd_path(const char *path) {
    if (!sd_on || !path) return 0;
    for (int i = 0; i < sd_n; i++) {
        size_t l = strlen(sd_mp[i]);
        if (!strncmp(path, sd_mp[i], l) && (path[l] == '/' || !path[l])) return 1;
    }
    return 0;
}

void sd_hold(void) {
    if (!sd_on) return;
    pthread_mutex_lock(&sd_lock);
    if (sd_count++ == 0)
        for (int i = 0; i < sd_n; i++)
            if (!sd_rw[i]) {
                if (sd_remount(i, 0) == 0) sd_rw[i] = 1;
                else logw("SD card: could not make %s writable (%s)", sd_mp[i], strerror(errno));
            }
    pthread_mutex_unlock(&sd_lock);
}

void sd_release(void) {
    if (!sd_on) return;
    pthread_mutex_lock(&sd_lock);
    int last = sd_count == 1;
    pthread_mutex_unlock(&sd_lock);
    if (last && log_kept()) log_write_kept();       /* (the log too, while it's writable) */
    pthread_mutex_lock(&sd_lock);
    if (sd_count > 0 && --sd_count == 0) sd_since = mono();
    pthread_mutex_unlock(&sd_lock);
}

/* Its own thread (never holding up the live view or the start-up): read-only
 * again once nothing has been written for 2 s - not before the start-up's
 * own writes are done (sd_started), as making a drive read-only takes a
 * moment - and the log kept in memory written now and then. */

static void sd_tick(void) {
    double now = mono();
    pthread_mutex_lock(&log_lock);
    /* (an error at once - well, within seconds; the rest with the next save,
     * or every half hour) */
    int due = log_buffered && log_path[0] && log_n &&
              (log_urgent ? now - log_written > 3 : now - log_written > 1800 || log_n > (size_t)sd_lines_kb * 1024);
    pthread_mutex_unlock(&log_lock);
    if (due) {
        sd_hold();
        log_write_kept();
        sd_release();
    }
    pthread_mutex_lock(&sd_lock);
    if (!sd_count && now - sd_since >= 2.0 && now >= sd_retry && now >= sd_not_before) {
        static int warned;
        int any = 0;
        for (int i = 0; i < sd_n; i++)
            if (sd_rw[i]) {
                if (sd_remount(i, 1) == 0) {
                    sd_rw[i] = 0;
                } else {                             /* (busy - a file still open: try again soon) */
                    if (!warned++) logw("SD card: %s still in use (%s) - read-only a little later", sd_mp[i],
                                        strerror(errno));
                    any = 1;
                }
            }
        if (any) sd_retry = now + 5;
        else warned = 0;
    }
    pthread_mutex_unlock(&sd_lock);
}

static void *sd_thread(void *arg) {
    (void)arg;
    pthread_mutex_lock(&sd_lock);
    while (!sd_quit) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 250000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&sd_cond, &sd_lock, &ts);
        if (sd_quit) break;
        pthread_mutex_unlock(&sd_lock);
        sd_tick();
        pthread_mutex_lock(&sd_lock);
    }
    pthread_mutex_unlock(&sd_lock);
    return NULL;
}

/* the start-up's own writes are done: read-only from 2 s later */
void sd_started(void) {
    pthread_mutex_lock(&sd_lock);
    sd_not_before = 0;
    pthread_mutex_unlock(&sd_lock);
}

int sd_writable(void) {
    if (!sd_on) return 0;
    pthread_mutex_lock(&sd_lock);
    int w = 0;
    for (int i = 0; i < sd_n; i++) w |= sd_rw[i];
    pthread_mutex_unlock(&sd_lock);
    return w;
}

/* leaving: everything written, the drives writable again (for the start-up
 * steps that may follow - an update, a card image...) */
void sd_end(void) {
    if (!sd_on) return;
    pthread_mutex_lock(&sd_lock);
    sd_quit = 1;
    pthread_cond_signal(&sd_cond);
    pthread_mutex_unlock(&sd_lock);
    pthread_join(sd_th, NULL);
    log_flush();
    pthread_mutex_lock(&sd_lock);
    for (int i = 0; i < sd_n; i++)
        if (!sd_rw[i] && sd_remount(i, 0) == 0) sd_rw[i] = 1;
    sd_on = 0;
    pthread_mutex_unlock(&sd_lock);
    pthread_mutex_lock(&log_lock);
    log_buffered = 0;
    pthread_mutex_unlock(&log_lock);
}

#define LOGFN(name, level) \
    void name(const char *fmt, ...) { va_list ap; va_start(ap, fmt); log_line(level, fmt, ap); va_end(ap); }
LOGFN(logi, "INFO")
LOGFN(logw, "WARNING")
LOGFN(loge, "ERROR")

/* ------------------------------------------------------------------ Buf */
void buf_add(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 256;
        b->p = realloc(b->p, b->cap);
    }
    memcpy(b->p + b->n, d, n);
    b->n += n;
}
void buf_u8(Buf *b, int v) { uint8_t c = (uint8_t)v; buf_add(b, &c, 1); }
void buf_be16(Buf *b, int v) { uint8_t c[2] = {(uint8_t)(v >> 8), (uint8_t)v}; buf_add(b, c, 2); }
void buf_le16(Buf *b, int v) { uint8_t c[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; buf_add(b, c, 2); }
void buf_be32(Buf *b, uint32_t v) {
    uint8_t c[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    buf_add(b, c, 4);
}
void buf_free(Buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}
