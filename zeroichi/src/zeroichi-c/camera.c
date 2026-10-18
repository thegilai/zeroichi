#define _GNU_SOURCE
#include "camera.h"
#include <errno.h>
#include <stddef.h>
#include <sys/un.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include "util.h"

struct dma_buf_sync { uint64_t flags; };
#define DMA_BUF_SYNC_READ (1 << 0)
#define DMA_BUF_SYNC_START (0 << 2)
#define DMA_BUF_SYNC_END (1 << 2)
#define DMA_BUF_IOCTL_SYNC _IOW('b', 0, struct dma_buf_sync)

typedef struct {
    int nplanes;
    int fd[3];
    uint32_t off[3], len[3];
    uint8_t *map[3];
    size_t maplen[3];
} ReqStream;

typedef struct {
    int valid, nstreams;
    ReqStream s[2];
} Req;

static int sock = -1, pid = -1, swap_uv, attached;
static int ready, failed, configured, pending_conf, paused, searching;
static char err[256];
char cam_ready_info[256];
static CamStream S[2];
static int nreq, reqs_seen;
static Req R[CAM_MAXREQ];
/* frames received and not taken yet, oldest first */
static int fifo[CAM_MAXREQ], nfifo;
static CamFrame frames[CAM_MAXREQ];

int cam_socket(void) { return sock; }
int cam_pid(void) { return pid; }
void cam_swap_uv(int on) { swap_uv = on; }
int cam_ready(void) { return ready; }
int cam_failed(void) { return failed; }
int cam_searching(void) { return searching && !ready; }
int cam_configured(void) { return configured; }
const char *cam_error(void) { return err; }
const CamStream *cam_main(void) { return &S[0]; }
const CamStream *cam_lores(void) { return S[1].w ? &S[1] : NULL; }
int cam_paused(void) { return paused > 0; }

static void sendmsg_text(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void sendmsg_text(const char *fmt, ...) {
    char b[256];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    if (sock >= 0 && n > 0) send(sock, b, (size_t)n, MSG_NOSIGNAL);
}

static void drop_all(void);

int cam_spawn(const char *const argv[]) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) return -1;
    pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        dup2(sv[1], 3);                          /* (dup2 clears close-on-exec) */
        for (int fd = 4; fd < 64; fd++) close(fd);
        execv(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(sv[1]);
    sock = sv[0];
    attached = 0;
    ready = failed = configured = searching = 0;
    return 0;
}

/* the camera server the fast start has started already (camd.py --listen):
 * connect to it (it may still be setting itself up: try for `wait` s) */
int cam_attach(const char *name, double wait) {
    double end = mono() + wait;
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    size_t n = strlen(name);
    if (n + 1 >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path + 1, name, n);                   /* (an "abstract" name: no file) */
    socklen_t len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + n);
    for (;;) {
        int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (s < 0) return -1;
        if (connect(s, (struct sockaddr *)&a, len) == 0) {
            struct ucred cr;
            socklen_t cl = sizeof cr;
            pid = getsockopt(s, SOL_SOCKET, SO_PEERCRED, &cr, &cl) == 0 ? cr.pid : -1;
            sock = s;
            attached = 1;
            ready = failed = configured = searching = 0;
            return 0;
        }
        close(s);
        if (mono() >= end) return -1;
        msleep(50);
    }
}

/* the server has ended (our own child: reaped; the fast start's: gone or a zombie) */
static int server_gone(void) {
    if (!attached) return waitpid(pid, NULL, WNOHANG) == pid;
    char p[64];
    snprintf(p, sizeof p, "/proc/%d/stat", pid);
    char *t = read_file(p, NULL);
    if (!t) return 1;
    char *r = strrchr(t, ')');
    int z = r && (r[2] == 'Z' || r[2] == 'X');
    free(t);
    return z;
}

static void unmap_all(void) {
    for (int i = 0; i < CAM_MAXREQ; i++) {
        for (int s = 0; s < R[i].nstreams; s++) {
            ReqStream *rs = &R[i].s[s];
            for (int p = 0; p < rs->nplanes; p++) {
                int shared = 0;
                for (int q = 0; q < p; q++) shared |= rs->fd[q] == rs->fd[p];
                if (rs->map[p] && !shared) munmap(rs->map[p], rs->maplen[p]);
                if (rs->fd[p] >= 0 && !shared) close(rs->fd[p]);
            }
        }
        memset(&R[i], 0, sizeof R[i]);
    }
    nfifo = 0;
}

static void got_req(char *text, int *fds, int nfds) {
    /* REQ k nstreams np off len off len ... np off len ... */
    char *save, *t = strtok_r(text, " ", &save);
    t = strtok_r(NULL, " ", &save);
    int k = atoi(t);
    int ns = atoi(strtok_r(NULL, " ", &save));
    if (k < 0 || k >= CAM_MAXREQ || ns > 2) return;
    Req *r = &R[k];
    memset(r, 0, sizeof *r);
    r->nstreams = ns;
    int fi = 0;
    for (int s = 0; s < ns; s++) {
        ReqStream *rs = &r->s[s];
        rs->nplanes = atoi(strtok_r(NULL, " ", &save));
        if (rs->nplanes > 3) rs->nplanes = 3;
        for (int p = 0; p < rs->nplanes; p++) {
            rs->off[p] = (uint32_t)strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            rs->len[p] = (uint32_t)strtoul(strtok_r(NULL, " ", &save), NULL, 10);
            rs->fd[p] = fi < nfds ? fds[fi++] : -1;
        }
        /* the same DMA buffer for all planes (normal): one mapping */
        for (int p = 0; p < rs->nplanes; p++) {
            int same = -1;
            for (int q = 0; q < p; q++) {
                struct stat a, b;
                if (fstat(rs->fd[q], &a) == 0 && fstat(rs->fd[p], &b) == 0 && a.st_ino == b.st_ino &&
                    a.st_dev == b.st_dev)
                    same = q;
            }
            if (same >= 0) {
                if (rs->fd[p] != rs->fd[same]) close(rs->fd[p]);
                rs->fd[p] = rs->fd[same];
                rs->map[p] = rs->map[same];
                rs->maplen[p] = rs->maplen[same];
                continue;
            }
            size_t len = 0;
            for (int q = p; q < rs->nplanes; q++) {
                size_t e = (size_t)rs->off[q] + rs->len[q];
                if (e > len) len = e;
            }
            void *m = mmap(NULL, len, PROT_READ, MAP_SHARED, rs->fd[p], 0);
            if (m == MAP_FAILED) {
                logw("Camera buffer could not be mapped: %s", strerror(errno));
                m = NULL;
            }
            rs->map[p] = m;
            rs->maplen[p] = len;
        }
    }
    r->valid = 1;
    reqs_seen++;
    if (reqs_seen == nreq && pending_conf) {
        configured = 1;
        pending_conf = 0;
    }
}

static void planes_of(const ReqStream *rs, const CamStream *cs, CamPlanes *p) {
    memset(p, 0, sizeof *p);
    if (!rs || rs->nplanes < 1 || !rs->map[0]) return;
    p->w = cs->w;
    p->h = cs->h;
    p->stride = cs->stride;
    p->frame_size = cs->frame_size;
    p->fd = rs->fd[0];
    p->y = rs->map[0] + rs->off[0];
    if (rs->nplanes >= 3) {
        p->u = rs->map[1] + rs->off[1] - (rs->map[1] == rs->map[0] ? 0 : 0);
        p->v = rs->map[2] + rs->off[2];
    } else {                                     /* one plane: work out where U, V are */
        p->u = p->y + (size_t)cs->stride * cs->h;
        p->v = p->u + (size_t)(cs->stride / 2) * (cs->h / 2);
    }
    if (swap_uv) {                               /* (settings.txt swap_rb = yes) */
        const uint8_t *t = p->u;
        p->u = p->v;
        p->v = t;
    }
}

static void handle(char *text, int *fds, int nfds) {
    if (!strncmp(text, "FRAME ", 6)) {
        int k, seq, exp = 0;
        long long ts;
        double gain = 0;
        if (sscanf(text + 6, "%d %d %lld %d %lf", &k, &seq, &ts, &exp, &gain) < 3) return;
        if (k < 0 || k >= CAM_MAXREQ || !R[k].valid) {
            sendmsg_text("DONE %d", k);
            return;
        }

        CamFrame *f = &frames[k];
        memset(f, 0, sizeof *f);
        f->cookie = k;
        f->seq = seq;
        f->ts_us = ts;
        f->exposure_us = exp;
        f->gain = gain;
        planes_of(&R[k].s[0], &S[0], &f->main);
        if (R[k].nstreams > 1) planes_of(&R[k].s[1], &S[1], &f->lores);
        if (nfifo < CAM_MAXREQ) fifo[nfifo++] = k;
        else sendmsg_text("DONE %d", k);
    } else if (!strncmp(text, "REQ ", 4)) {
        got_req(text, fds, nfds);
        return;
    } else if (!strncmp(text, "CONFIGURED ", 11)) {
        unmap_all();
        sscanf(text + 11, "%d %d %d %d %d %d %d %d %d", &S[0].w, &S[0].h, &S[0].stride, &S[0].frame_size,
               &S[1].w, &S[1].h, &S[1].stride, &S[1].frame_size, &nreq);
        if (nreq > CAM_MAXREQ) nreq = CAM_MAXREQ;
        reqs_seen = 0;
    } else if (!strncmp(text, "READY", 5)) {
        ready = 1;
        snprintf(cam_ready_info, sizeof cam_ready_info, "%s", text + 5 + (text[5] == ' '));
    } else if (!strncmp(text, "LOG ", 4)) {
        logi("Camera: %s", text + 4);
        if (strstr(text, "not found yet")) searching = 1;
    } else if (!strncmp(text, "ERROR", 5)) {
        failed = 1;
        snprintf(err, sizeof err, "%s", text + 5 + (text[5] == ' '));
        loge("Camera: %s", err);
    }
    for (int i = 0; i < nfds; i++) close(fds[i]);     /* (fds not for us) */
}

int cam_read(void) {
    if (sock < 0) return 0;
    for (;;) {
        char buf[4096];
        char cbuf[CMSG_SPACE(sizeof(int) * 16)];
        struct iovec iov = {buf, sizeof buf - 1};
        struct msghdr m = {0};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = cbuf;
        m.msg_controllen = sizeof cbuf;
        ssize_t n = recvmsg(sock, &m, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) {                            /* the server has ended */
            if (!failed) {
                failed = 1;
                snprintf(err, sizeof err, "camera server stopped");
                loge("Camera server stopped");
            }
            close(sock);
            sock = -1;
            break;
        }
        buf[n] = 0;
        int fds[16], nfds = 0;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                for (int i = 0; i < k && nfds < 16; i++) memcpy(&fds[nfds++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            }
        }
        handle(buf, fds, nfds);
    }
    return nfifo > 0;
}

int cam_take(CamFrame *f) {
    if (!nfifo) return 0;
    for (int i = 0; i < nfifo - 1; i++) sendmsg_text("DONE %d", fifo[i]);   /* (too late: dropped) */
    *f = frames[fifo[nfifo - 1]];
    nfifo = 0;
    return 1;
}

int cam_take_all(CamFrame *f, int max) {
    int n = 0;
    for (int i = 0; i < nfifo; i++) {
        if (n < max) f[n++] = frames[fifo[i]];
        else sendmsg_text("DONE %d", fifo[i]);
    }
    nfifo = 0;
    return n;
}

void cam_release_raw(int cookie) { sendmsg_text("DONE %d", cookie); }

int cam_wait_frame(CamFrame *f, double timeout) {
    double end = mono() + timeout;
    while (sock >= 0) {
        if (cam_read() && cam_take(f)) return 1;
        double left = end - mono();
        if (left <= 0) break;
        struct pollfd p = {sock, POLLIN, 0};
        poll(&p, 1, (int)(left * 1000) + 1);
    }
    return 0;
}

void cam_release(int cookie) {
    for (int i = 0; i < nfifo; i++)
        if (fifo[i] == cookie) {
            memmove(&fifo[i], &fifo[i + 1], sizeof(int) * (nfifo - i - 1));
            nfifo--;
            break;
        }
    sendmsg_text("DONE %d", cookie);
}

static void drop_all(void) {
    for (int i = 0; i < nfifo; i++) sendmsg_text("DONE %d", fifo[i]);
    nfifo = 0;
}

int cam_configure(int mw, int mh, int lw, int lh, int hflip, int vflip, int buffers, double fps,
                  double timeout) {
    drop_all();
    configured = 0;
    pending_conf = 1;
    sendmsg_text("CONF %d %d %d %d %d %d %d %.3f", mw, mh, lw, lh, hflip, vflip, buffers, fps);
    if (timeout <= 0) return 0;
    double end = mono() + timeout;
    while (!configured && !failed && sock >= 0) {
        double left = end - mono();
        if (left <= 0) return -1;
        struct pollfd p = {sock, POLLIN, 0};
        poll(&p, 1, (int)(left * 1000) + 1);
        cam_read();
    }
    return configured ? 0 : -1;
}

void cam_begin_read(const CamPlanes *p) {
    struct dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
    if (p->fd >= 0 && p->y) ioctl(p->fd, DMA_BUF_IOCTL_SYNC, &s);
}

void cam_end_read(const CamPlanes *p) {
    struct dma_buf_sync s = {DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ};
    if (p->fd >= 0 && p->y) ioctl(p->fd, DMA_BUF_IOCTL_SYNC, &s);
}

void cam_set_ev(double ev) { sendmsg_text("CTRL ExposureValue %.2f", ev); }

/* (v54) white balance: a preset's fixed colour gains, worked out by the camera
 * server from the camera's tuning file (whitebalance.py) - or Auto */

void cam_set_fps(double fps) {
    int us = (int)(1e6 / fps);
    sendmsg_text("CTRL FrameDurationLimits %d %d", us, us);
}

/* (v53) the time per picture between min_us and max_us: the camera takes the
 * shortest that its exposure allows (longer ones only when it's dark) */
void cam_set_frame_limits(int min_us, int max_us) {
    sendmsg_text("CTRL FrameDurationLimits %d %d", min_us, max_us);
}

void cam_pause(void) {
    if (paused++ == 0) {
        drop_all();
        sendmsg_text("STOP");
    }
}

void cam_resume(void) {
    if (paused <= 0) return;
    if (--paused == 0) sendmsg_text("START");
}

/* (v50) ends the camera server at once, without waiting for it: Zero Ichi is
 * about to be restarted (an update) and the server may still be loading the
 * camera library, which it wouldn't leave to read a QUIT - about 2 s spent
 * waiting at every update. (The fast start ends its own server anyway.) */
void cam_abandon(void) {
    if (sock >= 0) close(sock);
    sock = -1;
    if (pid > 0) {
        kill(pid, SIGTERM);
        for (int i = 0; !attached && i < 50 && !server_gone(); i++) msleep(10);   /* (our child: reaped) */
    }
    pid = -1;
}

void cam_quit(void) {
    if (sock < 0 && pid <= 0) return;
    if (sock >= 0) sendmsg_text("QUIT");
    /* wait for it to end (the camera is free then): QUIT, then TERM, then KILL */
    for (int i = 0; pid > 0 && i < 70; i++) {
        if (server_gone()) {
            pid = -1;
            break;
        }
        if (i == 40) kill(pid, SIGTERM);
        if (i == 60) kill(pid, SIGKILL);
        msleep(50);
    }
    if (sock >= 0) close(sock);
    sock = -1;
}
