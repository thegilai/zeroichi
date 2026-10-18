#include "jpeg.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/mman.h>
#include <unistd.h>
#include <jpeglib.h>
#include "looks.h"
#include "meta.h"

typedef struct {
    struct jpeg_error_mgr pub;
    jmp_buf jb;
    char msg[JMSG_LENGTH_MAX];
} Err;

static void err_exit(j_common_ptr c) {
    Err *e = (Err *)c->err;
    (*c->err->format_message)(c, e->msg);
    longjmp(e->jb, 1);
}

static void err_quiet(j_common_ptr c, int level) { (void)c; (void)level; }

/* -------------------------------------------------------------- encoding */
int jpeg_encode_yuv(const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v, int cstride,
                    int w, int h, int quality, Buf *out) {
    struct jpeg_compress_struct c;
    Err e;
    unsigned char *mem = NULL;
    unsigned long memlen = 0;
    int pw = (w + 15) & ~15, cw = pw / 2;
    uint8_t *scratch = malloc((size_t)pw * 16 + (size_t)cw * 16);
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = err_exit;
    e.pub.emit_message = err_quiet;
    if (setjmp(e.jb)) {
        jpeg_destroy_compress(&c);
        free(mem);
        free(scratch);
        logw("JPEG: %s", e.msg);
        return -1;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &memlen);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_YCbCr;
    jpeg_set_defaults(&c);
    jpeg_set_colorspace(&c, JCS_YCbCr);
    jpeg_set_quality(&c, quality, TRUE);
    c.raw_data_in = TRUE;
    c.comp_info[0].h_samp_factor = c.comp_info[0].v_samp_factor = 2;
    for (int i = 1; i < 3; i++) c.comp_info[i].h_samp_factor = c.comp_info[i].v_samp_factor = 1;
    c.dct_method = JDCT_ISLOW;
    jpeg_start_compress(&c, TRUE);
    JSAMPROW yr[16], ur[8], vr[8];
    JSAMPARRAY planes[3] = {yr, ur, vr};
    /* rows are padded to whole 16-pixel blocks (the last pixel repeated) */
    uint8_t *ypad = scratch, *cpad = scratch + (size_t)pw * 16;
    int hh = h / 2;
    for (int row = 0; row < h; row += 16) {
        for (int i = 0; i < 16; i++) {
            int r = row + i < h ? row + i : h - 1;
            if (pw == w) {
                yr[i] = (JSAMPROW)(y + (size_t)r * ystride);
            } else {
                uint8_t *d = ypad + (size_t)i * pw;
                memcpy(d, y + (size_t)r * ystride, w);
                memset(d + w, d[w - 1], pw - w);
                yr[i] = d;
            }
        }
        for (int k = 0; k < 2; k++) {
            const uint8_t *src = k ? v : u;
            JSAMPROW *rows = k ? vr : ur;
            for (int i = 0; i < 8; i++) {
                int r = row / 2 + i < hh ? row / 2 + i : hh - 1;
                if (cw == w / 2) {
                    rows[i] = (JSAMPROW)(src + (size_t)r * cstride);
                } else {
                    uint8_t *d = cpad + (size_t)(k * 8 + i) * cw;
                    memcpy(d, src + (size_t)r * cstride, w / 2);
                    memset(d + w / 2, d[w / 2 - 1], cw - w / 2);
                    rows[i] = d;
                }
            }
        }
        jpeg_write_raw_data(&c, planes, 16);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    out->n = 0;
    buf_add(out, mem, memlen);
    free(mem);
    free(scratch);
    return 0;
}

int jpeg_encode_rgb(Img rgb, int quality, Buf *out) {
    struct jpeg_compress_struct c;
    Err e;
    unsigned char *mem = NULL;
    unsigned long memlen = 0;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = err_exit;
    e.pub.emit_message = err_quiet;
    if (setjmp(e.jb)) {
        jpeg_destroy_compress(&c);
        free(mem);
        logw("JPEG: %s", e.msg);
        return -1;
    }
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &mem, &memlen);
    c.image_width = (JDIMENSION)rgb.w;
    c.image_height = (JDIMENSION)rgb.h;
    c.input_components = rgb.c;
    c.in_color_space = rgb.c == 3 ? JCS_RGB : JCS_GRAYSCALE;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality, TRUE);
    if (rgb.c == 3) {
        c.comp_info[0].h_samp_factor = c.comp_info[0].v_samp_factor = 2;
    }
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW r = (JSAMPROW)PX(rgb, 0, c.next_scanline);
        jpeg_write_scanlines(&c, &r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    out->n = 0;
    buf_add(out, mem, memlen);
    free(mem);
    return 0;
}

/* -------------------------------------------------------------- decoding */
int jpeg_needs_huffman(const uint8_t *d, size_t n) {
    size_t lim = n < 4096 ? n : 4096;
    for (size_t i = 0; i + 1 < lim; i++) {
        if (d[i] != 0xFF) continue;
        if (d[i + 1] == 0xC4) return 0;
        if (d[i + 1] == 0xDA) return 1;
    }
    return 0;
}

void jpeg_add_huffman(const uint8_t *d, size_t n, Buf *out) {
    out->n = 0;
    if (!jpeg_needs_huffman(d, n)) {
        buf_add(out, d, n);
        return;
    }
    buf_add(out, d, 2);
    buf_add(out, JPEG_STD_DHT, (size_t)JPEG_STD_DHT_LEN);
    buf_add(out, d + 2, n - 2);
}

static int start_decode(struct jpeg_decompress_struct *c, const uint8_t *data, size_t n, int min_w,
                        int min_h) {
    Buf fixed = {0};
    (void)fixed;
    jpeg_mem_src(c, data, (unsigned long)n);
    if (jpeg_read_header(c, TRUE) != JPEG_HEADER_OK) return -1;
    int denom = 1;
    if (min_w > 0 || min_h > 0) {
        for (int d = 8; d >= 2; d /= 2) {
            int ow = ((int)c->image_width + d - 1) / d, oh = ((int)c->image_height + d - 1) / d;
            if (ow >= min_w && oh >= min_h) {
                denom = d;
                break;
            }
        }
    }
    c->scale_num = 1;
    c->scale_denom = (unsigned)denom;
    return 0;
}

int jpeg_size(const uint8_t *data, size_t n, int *w, int *h) {
    struct jpeg_decompress_struct c;
    Err e;
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = err_exit;
    e.pub.emit_message = err_quiet;
    if (setjmp(e.jb)) {
        jpeg_destroy_decompress(&c);
        return -1;
    }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, data, (unsigned long)n);
    jpeg_read_header(&c, TRUE);
    *w = (int)c.image_width;
    *h = (int)c.image_height;
    jpeg_destroy_decompress(&c);
    return 0;
}

static Img decode(const uint8_t *data_in, size_t n_in, int min_w, int min_h, J_COLOR_SPACE cs, int comps) {
    struct jpeg_decompress_struct c;
    Err e;
    /* (kept in memory, not registers: still right after an error's longjmp) */
    Img *volatile out = calloc(1, sizeof(Img));
    Buf *volatile fixed = calloc(1, sizeof(Buf));
    const uint8_t *volatile data = data_in;
    volatile size_t n = n_in;
    Img result = {0};
    if (jpeg_needs_huffman(data, n)) {             /* (hardware MJPEG frames) */
        jpeg_add_huffman(data, n, fixed);
        data = fixed->p;
        n = fixed->n;
    }
    c.err = jpeg_std_error(&e.pub);
    e.pub.error_exit = err_exit;
    e.pub.emit_message = err_quiet;
    if (setjmp(e.jb)) {
        jpeg_destroy_decompress(&c);
        if (out->p) img_free(out);
        goto done;
    }
    jpeg_create_decompress(&c);
    if (start_decode(&c, data, n, min_w, min_h)) {
        jpeg_destroy_decompress(&c);
        goto done;
    }
    c.out_color_space = cs;
    c.dct_method = JDCT_ISLOW;
    jpeg_start_decompress(&c);
    *out = img_new((int)c.output_width, (int)c.output_height, comps);
    while (c.output_scanline < c.output_height) {
        JSAMPROW r = (JSAMPROW)PX(*out, 0, c.output_scanline);
        jpeg_read_scanlines(&c, &r, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    result = *out;
done:
    buf_free(fixed);
    free(fixed);
    free(out);
    return result;
}

Img jpeg_decode(const uint8_t *data, size_t n, int min_w, int min_h, int gray) {
    return decode(data, n, min_w, min_h, gray ? JCS_GRAYSCALE : JCS_RGB, gray ? 1 : 3);
}

int jpeg_decode_ycc(const uint8_t *data, size_t n, int min_w, int min_h, Img *y, Img *cb, Img *cr) {
    Img a = decode(data, n, min_w, min_h, JCS_YCbCr, 3);
    if (!a.p) return -1;
    *y = img_new(a.w, a.h, 1);
    *cb = img_new(a.w, a.h, 1);
    *cr = img_new(a.w, a.h, 1);
    for (int j = 0; j < a.h; j++) {
        const uint8_t *p = PX(a, 0, j);
        for (int x = 0; x < a.w; x++, p += 3) {
            PX(*y, x, j)[0] = p[0];
            PX(*cb, x, j)[0] = p[1];
            PX(*cr, x, j)[0] = p[2];
        }
    }
    img_free(&a);
    return 0;
}

/* ------------------------------------------------- the hardware encoder */
struct HwJpeg {
    int fd, w, h, yuv, stride, rows;
    size_t in_size;
    uint8_t *in;
    size_t in_len;
    uint8_t *out[2];
    size_t out_len[2];
};

static int xioctl(int fd, unsigned long req, void *arg, const char *step) {
    int r;
    do r = ioctl(fd, req, arg);
    while (r < 0 && errno == EINTR);
    if (r < 0) logi("Hardware JPEG: %s failed (%s)", step, strerror(errno));
    return r;
}

void hwjpeg_close(HwJpeg *e) {
    if (!e) return;
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(e->fd, VIDIOC_STREAMOFF, &t);
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(e->fd, VIDIOC_STREAMOFF, &t);
    if (e->in) munmap(e->in, e->in_len);
    for (int i = 0; i < 2; i++)
        if (e->out[i]) munmap(e->out[i], e->out_len[i]);
    struct v4l2_requestbuffers rq = {0};
    rq.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    rq.memory = V4L2_MEMORY_MMAP;
    ioctl(e->fd, VIDIOC_REQBUFS, &rq);
    rq.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(e->fd, VIDIOC_REQBUFS, &rq);
    close(e->fd);
    free(e);
}

static HwJpeg *hw_setup(int w, int h, int yuv, double bits) {
    if (getenv("ZEROICHI_SIM")) return NULL;
    int fd = open("/dev/video11", O_RDWR | O_CLOEXEC);
    if (fd < 0) return NULL;
    HwJpeg *e = calloc(1, sizeof *e);
    e->fd = fd;
    e->w = w;
    e->h = h;
    e->yuv = yuv;
    struct v4l2_control ctrl = {V4L2_CID_MPEG_VIDEO_BITRATE, 0};
    double br = (double)w * h * 15 * bits;
    ctrl.value = br > 25e6 ? 25000000 : (int)br;
    ioctl(fd, VIDIOC_S_CTRL, &ctrl);                 /* (not essential) */
    int row = yuv ? w : w * 3;
    struct v4l2_format f = {0};
    f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.pixelformat = yuv ? V4L2_PIX_FMT_YUV420 : V4L2_PIX_FMT_RGB24;
    f.fmt.pix_mp.plane_fmt[0].bytesperline = (unsigned)((row + 63) / 64 * 64);
    f.fmt.pix_mp.field = V4L2_FIELD_ANY;
    f.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
    if (yuv) f.fmt.pix_mp.quantization = V4L2_QUANTIZATION_FULL_RANGE;
    f.fmt.pix_mp.num_planes = 1;
    if (xioctl(fd, VIDIOC_S_FMT, &f, "set input format") || xioctl(fd, VIDIOC_G_FMT, &f, "read input format"))
        goto fail;
    e->stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;
    e->in_size = f.fmt.pix_mp.plane_fmt[0].sizeimage;
    e->rows = (int)f.fmt.pix_mp.height > h ? (int)f.fmt.pix_mp.height : h;
    size_t need = yuv ? (size_t)e->stride * e->rows * 3 / 2 : (size_t)e->stride * h;
    if (e->stride < row || (int)f.fmt.pix_mp.width != w || (int)f.fmt.pix_mp.height < h || e->in_size < need) {
        logi("Hardware JPEG: can't take %dx%d %s", w, h, yuv ? "yuv420" : "rgb24");
        goto fail;
    }
    memset(&f, 0, sizeof f);
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = (unsigned)w;
    f.fmt.pix_mp.height = (unsigned)h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_MJPEG;
    f.fmt.pix_mp.field = V4L2_FIELD_ANY;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 512 << 10;
    if (xioctl(fd, VIDIOC_S_FMT, &f, "set JPEG format")) goto fail;
    for (int pass = 0; pass < 2; pass++) {
        int type = pass ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        struct v4l2_requestbuffers rq = {0};
        rq.count = pass ? 2 : 1;
        rq.type = (unsigned)type;
        rq.memory = V4L2_MEMORY_MMAP;
        if (xioctl(fd, VIDIOC_REQBUFS, &rq, "request buffers")) goto fail;
        for (unsigned i = 0; i < rq.count && i < 2; i++) {
            struct v4l2_buffer b = {0};
            struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
            b.type = (unsigned)type;
            b.memory = V4L2_MEMORY_MMAP;
            b.index = i;
            b.length = 1;
            b.m.planes = pl;
            if (xioctl(fd, VIDIOC_QUERYBUF, &b, "query buffer")) goto fail;
            void *m = mmap(NULL, pl[0].length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, pl[0].m.mem_offset);
            if (m == MAP_FAILED) goto fail;
            if (pass) {
                e->out[i] = m;
                e->out_len[i] = pl[0].length;
                if (xioctl(fd, VIDIOC_QBUF, &b, "queue output buffer")) goto fail;
            } else {
                e->in = m;
                e->in_len = pl[0].length;
            }
        }
    }
    int t = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &t, "start input")) goto fail;
    t = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(fd, VIDIOC_STREAMON, &t, "start output")) goto fail;
    return e;
fail:
    hwjpeg_close(e);
    return NULL;
}

static int wait_ev(int fd, short ev) {
    struct pollfd p = {fd, ev, 0};
    double end = mono() + 4.0;
    while (mono() < end) {
        int r = poll(&p, 1, 400);
        if (r > 0 && (p.revents & ev)) return 0;
        if (r > 0 && (p.revents & (POLLERR | POLLNVAL))) return -1;
        if (r > 0) msleep(2);
    }
    return -1;
}

static int hw_run(HwJpeg *e, Buf *out) {
    struct v4l2_buffer b = {0};
    struct v4l2_plane pl[VIDEO_MAX_PLANES] = {0};
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = 0;
    b.field = V4L2_FIELD_NONE;
    b.length = 1;
    b.m.planes = pl;
    pl[0].bytesused = (unsigned)(e->in_size < e->in_len ? e->in_size : e->in_len);
    pl[0].length = (unsigned)e->in_len;
    if (xioctl(e->fd, VIDIOC_QBUF, &b, "give frame to encoder")) return -1;
    if (wait_ev(e->fd, POLLIN)) return -1;
    struct v4l2_buffer c = {0};
    struct v4l2_plane cp[VIDEO_MAX_PLANES] = {0};
    c.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    c.memory = V4L2_MEMORY_MMAP;
    c.length = 1;
    c.m.planes = cp;
    if (xioctl(e->fd, VIDIOC_DQBUF, &c, "take JPEG")) return -1;
    if (c.index < 2) jpeg_add_huffman(e->out[c.index], cp[0].bytesused, out);
    unsigned idx = c.index;
    memset(&c, 0, sizeof c);
    memset(cp, 0, sizeof cp);
    c.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    c.memory = V4L2_MEMORY_MMAP;
    c.index = idx;
    c.length = 1;
    c.m.planes = cp;
    cp[0].length = (unsigned)e->out_len[idx < 2 ? idx : 0];
    if (xioctl(e->fd, VIDIOC_QBUF, &c, "return output buffer")) return -1;
    if (wait_ev(e->fd, POLLOUT)) return -1;
    memset(&b, 0, sizeof b);
    memset(pl, 0, sizeof pl);
    b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.length = 1;
    b.m.planes = pl;
    if (xioctl(e->fd, VIDIOC_DQBUF, &b, "take back input")) return -1;
    return out->n > 0 ? 0 : -1;
}

/* the encoder's own input planes (Y, then U and V at half size): a frame can
 * be made straight into them (no copying - see jm_planes) */
void hwjpeg_planes(HwJpeg *e, uint8_t **y, int *ystride, uint8_t **u, uint8_t **v, int *cstride) {
    int st = e->stride, cs = st / 2, rows = e->rows;
    *y = e->in;
    *ystride = st;
    *u = e->in + (size_t)st * rows;
    *v = *u + (size_t)cs * (rows / 2);
    *cstride = cs;
}

int hwjpeg_encode_yuv(HwJpeg *e, const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v,
                      int cstride, Buf *out) {
    uint8_t *Y, *U, *V;
    int st, cs;
    hwjpeg_planes(e, &Y, &st, &U, &V, &cs);
    for (int r = 0; r < e->h; r++) memcpy(Y + (size_t)r * st, y + (size_t)r * ystride, e->w);
    for (int r = 0; r < e->h / 2; r++) {
        memcpy(U + (size_t)r * cs, u + (size_t)r * cstride, e->w / 2);
        memcpy(V + (size_t)r * cs, v + (size_t)r * cstride, e->w / 2);
    }
    return hw_run(e, out);
}

int hwjpeg_encode_rgb(HwJpeg *e, Img rgb, Buf *out) {
    for (int r = 0; r < e->h; r++) memcpy(e->in + (size_t)r * e->stride, PX(rgb, 0, r), (size_t)e->w * 3);
    return hw_run(e, out);
}

/* the test picture: gradients, so a mix-up of rows, colours or sizes shows */
static int hw_test(HwJpeg *e) {
    int w = e->w, h = e->h;
    Img Y = img_new(w, h, 1), U = img_new(w / 2, h / 2, 1), V = img_new(w / 2, h / 2, 1), rgb = img_new(w, h, 3);
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) {
            int r = i * 255 / (w > 1 ? w - 1 : 1), g = j * 255 / (h > 1 ? h - 1 : 1), b = 96;
            uint8_t *p = PX(rgb, i, j);
            p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
            uint8_t ycc[3];
            rgb_px_to_ycc(r, g, b, ycc);
            PX(Y, i, j)[0] = ycc[0];
            if (!(i & 1) && !(j & 1) && i / 2 < w / 2 && j / 2 < h / 2) {
                PX(U, i / 2, j / 2)[0] = ycc[1];
                PX(V, i / 2, j / 2)[0] = ycc[2];
            }
        }
    Buf out = {0};
    int bad = e->yuv ? hwjpeg_encode_yuv(e, Y.p, Y.stride, U.p, V.p, U.stride, &out) : hwjpeg_encode_rgb(e, rgb, &out);
    if (!bad) {
        Img got = jpeg_decode(out.p, out.n, w / 8, h / 8, 0);
        if (!got.p) {
            bad = 1;
        } else {
            double diff = 0;
            for (int j = 0; j < got.h; j++)
                for (int i = 0; i < got.w; i++) {
                    const uint8_t *p = PX(got, i, j);
                    int sx = (int)((i + 0.5) * w / got.w), sy = (int)((j + 0.5) * h / got.h);
                    const uint8_t *q = PX(rgb, sx, sy);
                    diff += abs(p[0] - q[0]) + abs(p[1] - q[1]) + abs(p[2] - q[2]);
                }
            diff /= (double)got.w * got.h * 3;
            int ww, hh;
            if (jpeg_size(out.p, out.n, &ww, &hh) || ww != w || hh != h || diff > 12) {
                logi("Hardware JPEG: test picture came out wrong (%dx%d, diff %.0f)", ww, hh, diff);
                bad = 1;
            }
            img_free(&got);
        }
    }
    buf_free(&out);
    img_free(&Y); img_free(&U); img_free(&V); img_free(&rgb);
    return bad;
}

static struct { int w, h, yuv, state; } hw_known[32];        /* state 1 ok, -1 bad, 2 ok (checked before) */
static int hw_nknown, hw_fmt_fails[2], hw_dirty;
static pthread_mutex_t hw_lock = PTHREAD_MUTEX_INITIALIZER;

/* (v50) The sizes the hardware encoder was checked for (the test picture) are
 * remembered from one start to the next - the first photo or video of each
 * size after switching on no longer waits for the check. Kept with the
 * system's version: a system update checks again. Only good results are
 * kept (a failed check is tried again at the next start). */
static char hw_file[256];
static int hw_loaded;

static void kernel_release(char *out, size_t cap) {
    struct utsname u;
    snprintf(out, cap, "%s", uname(&u) == 0 ? u.release : "?");
}

void hwjpeg_remember(const char *path) {
    pthread_mutex_lock(&hw_lock);
    snprintf(hw_file, sizeof hw_file, "%s", path);
    pthread_mutex_unlock(&hw_lock);
}

static void hw_load_locked(void) {
    if (hw_loaded || !hw_file[0]) return;
    hw_loaded = 1;
    FILE *f = fopen(hw_file, "r");
    if (!f) return;
    char line[160], rel[128], want[128];
    kernel_release(want, sizeof want);
    if (fgets(line, sizeof line, f) && sscanf(line, "kernel %127s", rel) == 1 && !strcmp(rel, want)) {
        int w, h, yuv;
        while (fgets(line, sizeof line, f) && hw_nknown < 32)
            if (sscanf(line, "%d %d %d", &w, &h, &yuv) == 3 && w > 0 && h > 0 && (yuv == 0 || yuv == 1))
                hw_known[hw_nknown++] = (typeof(hw_known[0])){w, h, yuv, 2};
    }
    fclose(f);
}

int hwjpeg_unsaved(void) { return hw_dirty; }

/* write the list (the caller makes the system partition writable) */
int hwjpeg_save(void) {
    pthread_mutex_lock(&hw_lock);
    int r = -1;
    if (hw_file[0]) {
        char rel[128], tmp[300];
        kernel_release(rel, sizeof rel);
        snprintf(tmp, sizeof tmp, "%s.tmp", hw_file);
        FILE *f = fopen(tmp, "w");
        if (f) {
            fprintf(f, "kernel %s\n", rel);
            for (int k = 0; k < hw_nknown; k++)
                if (hw_known[k].state > 0) fprintf(f, "%d %d %d\n", hw_known[k].w, hw_known[k].h, hw_known[k].yuv);
            r = fflush(f) || fsync(fileno(f)) ? -1 : 0;
            if (fclose(f)) r = -1;
            if (!r && rename(tmp, hw_file)) r = -1;
            if (r) unlink(tmp);
        }
    }
    hw_dirty = 0;                                    /* (failed: not tried again until there's news) */
    pthread_mutex_unlock(&hw_lock);
    return r;
}

static HwJpeg *hwjpeg_open_locked(int w, int h, int yuv, double bits);
HwJpeg *hwjpeg_open(int w, int h, int yuv, double bits) {
    pthread_mutex_lock(&hw_lock);
    HwJpeg *e = hwjpeg_open_locked(w, h, yuv, bits);
    pthread_mutex_unlock(&hw_lock);
    return e;
}

static HwJpeg *hwjpeg_open_locked(int w, int h, int yuv, double bits) {
    int k;
    /* (v53) the encoder's driver loads just after the camera is ready (the
     * fast start): a photo before that - software this once, nothing noted */
    if (access("/dev/video11", F_OK) && !getenv("ZEROICHI_SIM")) return NULL;
    hw_load_locked();
    for (k = 0; k < hw_nknown; k++)
        if (hw_known[k].w == w && hw_known[k].h == h && hw_known[k].yuv == yuv) break;
    if (k < hw_nknown && hw_known[k].state < 0) return NULL;
    if (k == hw_nknown && hw_fmt_fails[yuv] >= 2) return NULL;
    HwJpeg *e = hw_setup(w, h, yuv, bits);
    if (e && k == hw_nknown) {
        if (hw_test(e)) {
            hwjpeg_close(e);
            e = NULL;
        } else {
            logi("Hardware JPEG in use for %dx%d %s", w, h, yuv ? "yuv420" : "rgb24");
        }
    } else if (e && hw_known[k].state == 2) {          /* (checked at an earlier start) */
        hw_known[k].state = 1;
        logi("Hardware JPEG in use for %dx%d %s (checked before)", w, h, yuv ? "yuv420" : "rgb24");
    } else if (!e && k < hw_nknown && hw_known[k].state == 2) {
        hw_known[k].state = -1;                          /* (doesn't open now: not kept) */
        hw_dirty = 1;
    }
    if (k == hw_nknown && hw_nknown < 32) {
        hw_known[hw_nknown++] = (typeof(hw_known[0])){w, h, yuv, e ? 1 : -1};
        if (!e) hw_fmt_fails[yuv]++;
        if (e) hw_dirty = 1;
    }
    return e;
}

/* ------------------------------------------------------------ JpegMaker */
void jm_init(JpegMaker *m, int w, int h, int quality, double bits) {
    memset(m, 0, sizeof *m);
    m->w = w;
    m->h = h;
    m->quality = quality;
    m->bits = bits;
}

int jm_yuv(JpegMaker *m, const uint8_t *y, int ystride, const uint8_t *u, const uint8_t *v, int cstride,
           Buf *out) {
    if (!m->tried[1]) {
        m->tried[1] = 1;
        m->hw[1] = hwjpeg_open(m->w, m->h, 1, m->bits);
    }
    if (m->hw[1]) {
        if (!hwjpeg_encode_yuv(m->hw[1], y, ystride, u, v, cstride, out)) {
            m->used_hw = 1;
            return 0;
        }
        logw("Hardware JPEG failed - compressing in software");
        hwjpeg_close(m->hw[1]);
        m->hw[1] = NULL;
    }
    m->used_sw = 1;
    return jpeg_encode_yuv(y, ystride, u, v, cstride, m->w, m->h, m->quality, out);
}

int jm_rgb(JpegMaker *m, Img rgb, Buf *out) {
    if (!m->tried[0]) {
        m->tried[0] = 1;
        m->hw[0] = hwjpeg_open(m->w, m->h, 0, m->bits);
    }
    if (m->hw[0]) {
        if (!hwjpeg_encode_rgb(m->hw[0], rgb, out)) {
            m->used_hw = 1;
            return 0;
        }
        logw("Hardware JPEG failed - compressing in software");
        hwjpeg_close(m->hw[0]);
        m->hw[0] = NULL;
    }
    m->used_sw = 1;
    return jpeg_encode_rgb(rgb, m->quality, out);
}

/* the planes to fill for jm_encode_planes(): the hardware encoder's own
 * input buffer when it works (no copying), else a buffer of the maker's own.
 * (Only written, never read back: the encoder's buffer is slow to read.) */
int jm_planes(JpegMaker *m, uint8_t **y, int *ystride, uint8_t **u, uint8_t **v, int *cstride) {
    if (!m->tried[1]) {
        m->tried[1] = 1;
        m->hw[1] = hwjpeg_open(m->w, m->h, 1, m->bits);
    }
    if (m->hw[1]) {
        hwjpeg_planes(m->hw[1], y, ystride, u, v, cstride);
        m->planes_hw = 1;
        return 0;
    }
    int cw = m->w / 2, ch = m->h / 2;
    if (!m->sw) m->sw = malloc((size_t)m->w * m->h + 2 * (size_t)cw * ch + 16);
    if (!m->sw) return -1;
    *y = m->sw;
    *ystride = m->w;
    *u = m->sw + (size_t)m->w * m->h;
    *v = *u + (size_t)cw * ch;
    *cstride = cw;
    m->planes_hw = 0;
    return 0;
}

int jm_encode_planes(JpegMaker *m, Buf *out) {
    uint8_t *y, *u, *v;
    int ys, cs;
    if (m->planes_hw && m->hw[1]) {
        if (!hw_run(m->hw[1], out)) {
            m->used_hw = 1;
            return 0;
        }
        logw("Hardware JPEG failed - compressing in software");
        hwjpeg_planes(m->hw[1], &y, &ys, &u, &v, &cs);      /* (the frame is still there) */
        out->n = 0;
        int r = jpeg_encode_yuv(y, ys, u, v, cs, m->w, m->h, m->quality, out);
        hwjpeg_close(m->hw[1]);
        m->hw[1] = NULL;
        m->used_sw = 1;
        return r;
    }
    if (!m->sw) return -1;
    int cw = m->w / 2;
    y = m->sw;
    u = m->sw + (size_t)m->w * m->h;
    v = u + (size_t)cw * (m->h / 2);
    m->used_sw = 1;
    return jpeg_encode_yuv(y, m->w, u, v, cw, m->w, m->h, m->quality, out);
}

const char *jm_how(JpegMaker *m) {
    return m->used_hw && m->used_sw ? "hardware+software" : m->used_hw ? "hardware" : m->used_sw ? "software" : "-";
}

void jm_close(JpegMaker *m) {
    for (int i = 0; i < 2; i++) hwjpeg_close(m->hw[i]);
    m->hw[0] = m->hw[1] = NULL;
    free(m->sw);
    m->sw = NULL;
}

/* a photo's EXIF: the look (as its description: "Look: Super 8"), the
 * camera's maker and model and the program that made it - nothing else, no
 * date (see meta.h). Goes after the JFIF header (APP0), or right after the
 * start. The same bytes as the Python Zero Ichi's (movie.py's exif_segment). */
void jpeg_add_exif(const uint8_t *d, size_t n, const char *look, Buf *out) {
    char desc[96];
    snprintf(desc, sizeof desc, META_LOOK "%s", look ? look : "");
    /* ImageDescription, Make, Model, Software (in tag order) */
    const uint16_t all_tags[4] = {0x010E, 0x010F, 0x0110, 0x0131};
    const char *const all_texts[4] = {desc, META_MAKE, META_MODEL, META_SOFTWARE};
    int first = look && look[0] ? 0 : 1, cnt = 4 - first;
    const uint16_t *tags = all_tags + first;
    const char *const *texts = all_texts + first;
    Buf t = {0}, data = {0};
    buf_add(&t, "MM\0\x2a", 4);                     /* TIFF, big-endian */
    buf_be32(&t, 8);                                 /* IFD0 right after */
    buf_be16(&t, cnt);
    uint32_t off = 8 + 2 + (uint32_t)cnt * 12 + 4;   /* where the texts start */
    for (int i = 0; i < cnt; i++) {
        size_t len = strlen(texts[i]) + 1;
        buf_be16(&t, tags[i]);
        buf_be16(&t, 2);                             /* ASCII */
        buf_be32(&t, (uint32_t)len);
        if (len <= 4) {                              /* (short: in the entry itself) */
            uint8_t v[4] = {0, 0, 0, 0};
            memcpy(v, texts[i], len);
            buf_add(&t, v, 4);
            continue;
        }
        buf_be32(&t, off + (uint32_t)data.n);
        buf_add(&data, texts[i], len);
        if (len & 1) buf_u8(&data, 0);               /* (even offsets) */
    }
    buf_be32(&t, 0);                                 /* no next IFD */
    buf_add(&t, data.p, data.n);
    size_t at = 2;
    if (n >= 6 && d[2] == 0xFF && d[3] == 0xE0) at = 4 + ((size_t)d[4] << 8 | d[5]);
    if (n < 2 || at > n) at = n;
    buf_add(out, d, at);
    buf_u8(out, 0xFF);
    buf_u8(out, 0xE1);
    buf_be16(out, (int)(2 + 6 + t.n));
    buf_add(out, "Exif\0\0", 6);
    buf_add(out, t.p, t.n);
    buf_add(out, d + at, n - at);
    buf_free(&t);
    buf_free(&data);
}
