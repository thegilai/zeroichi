/* Zero Ichi - recording with the Pi's hardware MJPEG encoder: camera frames go to
 * it straight from the camera's buffers (no copies), and each compressed
 * frame is written to the video file by the encoder's own thread. */
#ifndef ZEROICHI_VENC_H
#define ZEROICHI_VENC_H
#include <stdint.h>
#include "mov.h"

typedef struct Venc Venc;
typedef void (*venc_release_fn)(int cookie);

Venc *venc_open(int w, int h, int stride, int frame_size, double fps, MovWriter *mov,
                venc_release_fn release);
/* give a camera frame (its DMA buffer) to the encoder; 0 = taken (released
 * later through the callback), -1 = not taken (release it yourself) */
int venc_put(Venc *e, int dmafd, int cookie, int64_t ts_us);
int venc_frames(Venc *e);
int venc_failed(Venc *e);
void venc_close(Venc *e);          /* finishes the frames in flight */
#endif
