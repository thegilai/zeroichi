#include "venc.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "util.h"

#define NOUT 16
#define NCAP 16

struct Venc {
    int fd, w, h;
    MovWriter *mov;
    venc_release_fn release;
    uint8_t *cap[NCAP];
    size_t caplen[NCAP];
    int out_cookie[NOUT];          /* camera frame in each input buffer (-1 = free) */
    int frame_size;
    pthread_t th;
    pthread_mutex_t lock;
    volatile int running, frames, failed, inflight;
    int64_t t0;
};

static int xi(int fd, unsigned long req, void *arg) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    return r;
}

static void *poll_thread(void *arg) {
    Venc *e = arg;
    struct pollfd p = {e->fd, POLLIN | POLLOUT, 0};
    while (e->running || e->inflight > 0) {
        int r = poll(&p, 1, 400);
        if (r == 0 && !e->running) break;          /* (the last frames never came back) */
        if (r <= 0) continue;
        if (p.revents & (POLLERR | POLLNVAL)) {
            e->failed = 1;
            break;
        }
        if (p.revents & POLLOUT) {                 /* an input frame is done: give it back */
            struct v4l2_buffer b = {0};
            struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
            b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            b.memory = V4L2_MEMORY_DMABUF;
            b.length = 1;
            b.m.planes = pl;
            if (xi(e->fd, VIDIOC_DQBUF, &b) == 0 && b.index < NOUT) {
                pthread_mutex_lock(&e->lock);
                int ck = e->out_cookie[b.index];
                e->out_cookie[b.index] = -1;
                e->inflight--;
                pthread_mutex_unlock(&e->lock);
                if (ck >= 0) e->release(ck);
            }
        }
        if (p.revents & POLLIN) {                  /* a compressed frame */
            struct v4l2_buffer b = {0};
            struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.length = 1;
            b.m.planes = pl;
            if (xi(e->fd, VIDIOC_DQBUF, &b) == 0 && b.index < NCAP) {
                int64_t ts = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
                if (e->t0 < 0) e->t0 = ts;
                if (pl[0].bytesused > 0 &&
                    mov_add(e->mov, e->cap[b.index], pl[0].bytesused, (ts - e->t0) / 1e6) == 0)
                    e->frames++;
                unsigned idx = b.index;
                memset(&b, 0, sizeof b);
                memset(pl, 0, sizeof pl);
                b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                b.memory = V4L2_MEMORY_MMAP;
                b.index = idx;
                b.length = 1;
                b.m.planes = pl;
                pl[0].length = (unsigned)e->caplen[idx];
                xi(e->fd, VIDIOC_QBUF, &b);
            }
        }
    }
    return NULL;
}

Venc *venc_open(int w, int h, int stride, int frame_size, double fps, MovWriter *mov,
                venc_release_fn release) {
    if (getenv("ZEROICHI_SIM")) return NULL;
    int fd = open("/dev/video11", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        logw("Hardware video: no encoder (%s)", strerror(errno));
        return NULL;
    }
    Venc *e = calloc(1, sizeof *e);
    e->fd = fd;
    e->w = w;
    e->h = h;
    e->mov = mov;
    e->release = release;
    e->frame_size = frame_size;
    e->t0 = -1;
    pthread_mutex_init(&e->lock, NULL);
    for (int i = 0; i < NOUT; i++) e->out_cookie[i] = -1;
    const char *step = "";
    /* quality: picamera2's MJPEG "HIGH" (40 Mbit/s at 1080p30, scaled) */
    struct v4l2_control c = {V4L2_CID_MPEG_VIDEO_BITRATE, (int)(40e6 * w * h * fps / (1920.0 * 1080 * 30))};
    xi(fd, VIDIOC_S_CTRL, &c);
    struct v4l2_format f = {0};
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    f.fmt.pix_mp.plane_fmt[0].bytesperline = (unsigned)stride;
    f.fmt.pix_mp.field = V4L2_FIELD_ANY;
    f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
    f.fmt.pix_mp.num_planes = 1;
    step = "input format";
    if (xi(fd, VIDIOC_S_FMT, &f)) goto fail;
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix_mp.field = V4L2_FIELD_ANY;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 512 << 10;
    step = "JPEG format";
    if (xi(fd, VIDIOC_S_FMT, &f)) goto fail;
    struct v4l2_requestbuffers rq = {0};
    rq.count = NOUT;
    rq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rq.memory = V4L2_MEMORY_DMABUF;
    step = "input buffers";
    if (xi(fd, VIDIOC_REQBUFS, &rq)) goto fail;
    memset(&rq, 0, sizeof rq);
    rq.count = NCAP;
    rq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rq.memory = V4L2_MEMORY_MMAP;
    step = "output buffers";
    if (xi(fd, VIDIOC_REQBUFS, &rq)) goto fail;
    for (unsigned i = 0; i < rq.count && i < NCAP; i++) {
        struct v4l2_buffer b = {0};
        struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.length = 1;
        b.m.planes = pl;
        step = "query buffer";
        if (xi(fd, VIDIOC_QUERYBUF, &b)) goto fail;
        void *m = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
        step = "map buffer";
        if (m == MAP_FAILED) goto fail;
        e->cap[i] = m;
        e->caplen[i] = pl[0].length;
        step = "queue buffer";
        if (xi(fd, VIDIOC_QBUF, &b)) goto fail;
    }
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    step = "start";
    if (xi(fd, VIDIOC_STREAMON, &t)) goto fail;
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xi(fd, VIDIOC_STREAMON, &t)) goto fail;
    e->running = 1;
    pthread_create(&e->th, NULL, poll_thread, e);
    return e;
fail:
    logw("Hardware video: %s failed (%s)", step, strerror(errno));
    for (int i = 0; i < NCAP; i++)
        if (e->cap[i]) munmap(e->cap[i], e->caplen[i]);
    close(fd);
    free(e);
    return NULL;
}

int venc_put(Venc *e, int dmafd, int cookie, int64_t ts_us) {
    if (!e || e->failed) return -1;
    pthread_mutex_lock(&e->lock);
    int idx = -1;
    for (int i = 0; i < NOUT; i++)
        if (e->out_cookie[i] < 0) {
            idx = i;
            break;
        }
    if (idx >= 0) {
        e->out_cookie[idx] = cookie;
        e->inflight++;
    }
    pthread_mutex_unlock(&e->lock);
    if (idx < 0) return -1;
    struct v4l2_buffer b = {0};
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_DMABUF;
    b.index = (unsigned)idx;
    b.field = V4L2_FIELD_NONE;
    b.length = 1;
    b.timestamp.tv_sec = ts_us / 1000000;
    b.timestamp.tv_usec = ts_us % 1000000;
    b.m.planes = pl;
    pl[0].m.fd = dmafd;
    pl[0].bytesused = (unsigned)e->frame_size;
    pl[0].length = (unsigned)e->frame_size;
    if (xi(e->fd, VIDIOC_QBUF, &b)) {
        logw("Hardware video: frame not taken (%s)", strerror(errno));
        pthread_mutex_lock(&e->lock);
        e->out_cookie[idx] = -1;
        e->inflight--;
        pthread_mutex_unlock(&e->lock);
        e->failed = 1;
        return -1;
    }
    return 0;
}

int venc_frames(Venc *e) { return e ? e->frames : 0; }
int venc_failed(Venc *e) { return e ? e->failed : 1; }

void venc_close(Venc *e) {
    if (!e) return;
    e->running = 0;
    pthread_join(e->th, NULL);
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xi(e->fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xi(e->fd, VIDIOC_STREAMOFF, &t);
    for (int i = 0; i < NOUT; i++)                 /* frames never returned: give them back */
        if (e->out_cookie[i] >= 0) e->release(e->out_cookie[i]);
    struct v4l2_requestbuffers rq = {0};
    rq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rq.memory = V4L2_MEMORY_DMABUF;
    xi(e->fd, VIDIOC_REQBUFS, &rq);
    for (int i = 0; i < NCAP; i++)
        if (e->cap[i]) munmap(e->cap[i], e->caplen[i]);
    rq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    rq.memory = V4L2_MEMORY_MMAP;
    xi(e->fd, VIDIOC_REQBUFS, &rq);
    close(e->fd);
    pthread_mutex_destroy(&e->lock);
    free(e);
}
