/* Zero Ichi - the camera, run by the camera server (camd.py) and read straight
 * from its frame buffers (shared memory, no copies). */
#ifndef ZEROICHI_CAMERA_H
#define ZEROICHI_CAMERA_H
#include <stddef.h>
#include <stdint.h>

#define CAM_MAXREQ 16

typedef struct {
    int w, h, stride, frame_size;        /* as the camera set it up */
} CamStream;

typedef struct {                          /* one frame's YUV420 planes */
    const uint8_t *y, *u, *v;
    int w, h, stride;                     /* (U, V: half size, stride / 2) */
    int fd;                               /* its DMA buffer (for the video encoder) */
    int frame_size;
} CamPlanes;

typedef struct {
    int cookie;
    int64_t ts_us;
    int seq;
    int exposure_us;
    double gain;
    CamPlanes main, lores;               /* lores.y == NULL: no small stream */
} CamFrame;

int cam_spawn(const char *const argv[]);  /* start the camera server */
int cam_attach(const char *name, double wait);   /* or use the one the fast start started */
int cam_socket(void);                     /* poll this for messages */
int cam_pid(void);
void cam_swap_uv(int on);                 /* red and blue swapped (some cameras) */
/* set up streams (main + optional small "lores" one); waits for the camera
 * (up to `timeout` s). 0 = ok. */
int cam_configure(int mw, int mh, int lw, int lh, int hflip, int vflip, int buffers, double fps,
                  double timeout);
const CamStream *cam_main(void);
const CamStream *cam_lores(void);
int cam_configured(void);
int cam_ready(void);                      /* the server found the camera */
int cam_failed(void);                     /* the server gave up (no camera) */
int cam_searching(void);                  /* not found at the first look: still trying */
const char *cam_error(void);
/* read what the server sent; returns 1 if a new frame is waiting (older
 * frames not picked up yet are handed back) */
int cam_read(void);
/* take the newest frame (0 = none); give it back with cam_release() */
int cam_take(CamFrame *f);
int cam_take_all(CamFrame *f, int max);    /* every frame received, oldest first */
void cam_release_raw(int cookie);          /* (from another thread) */
int cam_wait_frame(CamFrame *f, double timeout);
void cam_release(int cookie);
void cam_begin_read(const CamPlanes *p);  /* DMA buffer cache sync (CPU reads) */
void cam_end_read(const CamPlanes *p);
void cam_set_ev(double ev);
void cam_set_fps(double fps);
void cam_set_frame_limits(int min_us, int max_us);   /* (v53) */
void cam_pause(void);
void cam_resume(void);
int cam_paused(void);
void cam_quit(void);
void cam_abandon(void);                  /* (no waiting: Zero Ichi restarts) */
extern char cam_ready_info[256];
#endif
