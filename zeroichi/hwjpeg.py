"""Compress RGB frames to JPEG with the Pi's hardware video encoder.

The same encoder (/dev/video11, used through V4L2) records Zero Ichi's videos via
picamera2. Here it compresses the frames of a video being "developed", which
on a Pi Zero takes a few milliseconds instead of 100-160 ms in software.

The ioctl sequence mirrors picamera2's V4L2Encoder (which is known to work on
the Pi). Input frames are given to the encoder in one of two ways: a buffer
the encoder provides ("mmap"), or - exactly like picamera2 - a DMA buffer
from the kernel's dma_heap ("dmabuf"). The first that works is used. If
neither works, or the test frame doesn't come out right, Zero Ichi simply
compresses in software. Every step is named in the log if it fails.
"""

import fcntl
import logging
import mmap
import os
import select

log = logging.getLogger("zeroichi")

DEVICE = "/dev/video11"
_LOGGED = {}
_VERIFIED = {}          # (w, h, fmt) -> buffer mode that passed the test picture


class HwJpeg:
    def __init__(self, width, height, fps=15, mode="mmap", bits=2.0, fmt="rgb24"):
        import ctypes
        import videodev2 as v
        self.v, self.ctypes = v, ctypes
        self.w, self.h, self.mode, self.bits = width, height, mode, bits
        self.fmt = fmt                  # "rgb24" (R,G,B bytes) or "yuv420" (Y, U, V planes)
        self.dma_fd = -1
        self.inbuf, self.outbufs = [], []
        self.vd = open(DEVICE, "rb+", buffering=0)
        try:
            self._setup(fps)
        except Exception:
            self.close()
            raise

    def _io(self, step, req, arg):
        """ioctl that says which step failed."""
        try:
            return fcntl.ioctl(self.vd, req, arg)
        except OSError as e:
            raise RuntimeError(f"{step}: {e}") from None

    @staticmethod
    def _map(fd, length, offset=0):
        # (keyword arguments: picamera2 passes prot/flags the other way round,
        #  which only works for buffers that are just read)
        return mmap.mmap(fd, length, flags=mmap.MAP_SHARED,
                         prot=mmap.PROT_READ | mmap.PROT_WRITE, offset=offset)

    def _planes(self):
        return (self.v.v4l2_plane * self.v.VIDEO_MAX_PLANES)()

    def _setup(self, fps):
        v, vd = self.v, self.vd
        # quality: about 2 bits per pixel (similar to Zero Ichi's software JPEGs)
        ctrl = v.v4l2_control()
        ctrl.id = v.V4L2_CID_MPEG_VIDEO_BITRATE
        ctrl.value = min(25_000_000, int(self.w * self.h * fps * self.bits))
        try:
            fcntl.ioctl(vd, v.VIDIOC_S_CTRL, ctrl)
        except OSError as e:                                  # not essential
            log.info("Hardware JPEG: quality setting not accepted (%s)", e)

        yuv = self.fmt == "yuv420"
        row = self.w if yuv else self.w * 3
        stride = (row + 63) // 64 * 64
        fmt = v.v4l2_format()
        fmt.type = v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
        fmt.fmt.pix_mp.width = self.w
        fmt.fmt.pix_mp.height = self.h
        # YUV420 = the camera's own format: no colour conversion anywhere
        fmt.fmt.pix_mp.pixelformat = v.V4L2_PIX_FMT_YUV420 if yuv else v.V4L2_PIX_FMT_RGB24
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = stride
        fmt.fmt.pix_mp.field = v.V4L2_FIELD_ANY
        fmt.fmt.pix_mp.colorspace = v.V4L2_COLORSPACE_JPEG
        if yuv and hasattr(v, "V4L2_QUANTIZATION_FULL_RANGE"):
            fmt.fmt.pix_mp.quantization = v.V4L2_QUANTIZATION_FULL_RANGE
        fmt.fmt.pix_mp.num_planes = 1
        self._io("set input format", v.VIDIOC_S_FMT, fmt)
        self._io("read input format", v.VIDIOC_G_FMT, fmt)   # what the driver accepted
        self.stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline
        self.in_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage
        self.rows = max(self.h, fmt.fmt.pix_mp.height)       # plane height in the buffer
        need = self.stride * self.rows * 3 // 2 if yuv else self.stride * self.h
        if self.stride < row or fmt.fmt.pix_mp.width != self.w \
                or fmt.fmt.pix_mp.height < self.h or self.in_size < need:
            raise RuntimeError(f"encoder can't take {self.w}x{self.h} {self.fmt}")

        fmt = v.v4l2_format()
        fmt.type = v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        fmt.fmt.pix_mp.width = self.w
        fmt.fmt.pix_mp.height = self.h
        fmt.fmt.pix_mp.pixelformat = v.V4L2_PIX_FMT_MJPEG
        fmt.fmt.pix_mp.field = v.V4L2_FIELD_ANY
        fmt.fmt.pix_mp.colorspace = v.V4L2_COLORSPACE_DEFAULT
        fmt.fmt.pix_mp.num_planes = 1
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline = 0
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 512 << 10
        self._io("set JPEG format", v.VIDIOC_S_FMT, fmt)

        if self.mode == "dmabuf":
            self.inbuf = self._dma_input()
        else:
            self.inbuf = self._buffers(v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, queue=False)
        self.outbufs = self._buffers(v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, queue=True)
        self._io("start input", v.VIDIOC_STREAMON, v.v4l2_buf_type(v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE))
        self._io("start output", v.VIDIOC_STREAMON, v.v4l2_buf_type(v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE))

    def _buffers(self, btype, queue):
        """Memory-mapped buffers (2 of them); capture buffers are queued now."""
        v = self.v
        what = "output" if queue else "input"
        req = v.v4l2_requestbuffers()
        req.count = 2
        req.type = btype
        req.memory = v.V4L2_MEMORY_MMAP
        self._io(f"request {what} buffers", v.VIDIOC_REQBUFS, req)
        bufs = []
        for i in range(req.count):
            b = v.v4l2_buffer()
            self.ctypes.memset(self.ctypes.byref(b), 0, self.ctypes.sizeof(b))
            planes = self._planes()
            b.type, b.memory, b.index, b.length = btype, v.V4L2_MEMORY_MMAP, i, 1
            b.m.planes = planes
            self._io(f"query {what} buffer", v.VIDIOC_QUERYBUF, b)
            length = b.m.planes[0].length
            try:
                mm = self._map(self.vd.fileno(), length, b.m.planes[0].m.mem_offset)
            except OSError as e:
                raise RuntimeError(f"map {what} buffer: {e}") from None
            bufs.append((mm, length))
            if queue:
                self._io(f"queue {what} buffer", v.VIDIOC_QBUF, b)
        return bufs

    def _dma_input(self):
        """One DMA buffer for the input frames - the way picamera2 feeds the
        encoder (camera buffers are DMA buffers too)."""
        v = self.v
        req = v.v4l2_requestbuffers()
        req.count = 1
        req.type = v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
        req.memory = v.V4L2_MEMORY_DMABUF
        self._io("request DMA input buffer", v.VIDIOC_REQBUFS, req)
        try:
            from picamera2.dma_heap import DmaHeap
            ufd = DmaHeap().alloc("zeroichi-jpeg", self.in_size)
            self.dma_fd = ufd.release()
            if self.dma_fd < 0:
                raise RuntimeError("no memory")
            mm = self._map(self.dma_fd, self.in_size)
        except Exception as e:
            raise RuntimeError(f"DMA buffer: {e}") from None
        return [(mm, self.in_size)]

    def _sync(self, end):
        """Tell the kernel when the CPU starts/stops writing a (cached) DMA buffer."""
        if self.dma_fd < 0:
            return
        try:
            from picamera2.dma_heap import (DMA_BUF_IOCTL_SYNC, DMA_BUF_SYNC_END,
                                            DMA_BUF_SYNC_START, DMA_BUF_SYNC_WRITE,
                                            dma_buf_sync)
            s = dma_buf_sync()
            s.flags = DMA_BUF_SYNC_WRITE | (DMA_BUF_SYNC_END if end else DMA_BUF_SYNC_START)
            fcntl.ioctl(self.dma_fd, DMA_BUF_IOCTL_SYNC, s)
        except Exception:
            pass

    def _wait(self, flag, seconds=4.0):
        """Wait for one kind of event only (POLLIN = compressed frame ready,
        POLLOUT = input buffer done). Watching both at once made the wait end
        early: "input done" arrives first and used up the checks."""
        import time
        p = select.poll()
        p.register(self.vd, flag)
        end = time.monotonic() + seconds
        seen = 0
        while True:
            left = end - time.monotonic()
            if left <= 0:
                raise TimeoutError(f"hardware encoder didn't answer (events seen: {seen:#x})")
            for _fd, ev in p.poll(int(left * 1000) + 1):
                seen |= ev
                if ev & flag:
                    return
                if ev & (select.POLLERR | select.POLLNVAL):
                    raise RuntimeError(f"hardware encoder error (events {ev:#x})")
            time.sleep(0.002)

    def encode(self, rgb):
        """rgb: numpy array (h, w, 3) uint8, R,G,B. Returns JPEG bytes."""
        import numpy as np
        if self.fmt != "rgb24":
            raise ValueError("this encoder takes YUV planes")
        mm, length = self.inbuf[0]
        self._sync(end=False)
        row = self.w * 3
        if self.stride == row:
            mm[:row * self.h] = memoryview(np.ascontiguousarray(rgb).reshape(-1))
        else:                                                 # padded rows: one numpy copy
            dst = np.frombuffer(mm, np.uint8, self.stride * self.h).reshape(self.h, self.stride)
            dst[:, :row] = rgb.reshape(self.h, row)
            del dst                                           # (release the mmap view)
        self._sync(end=True)
        return self._run(length)

    def encode_yuv(self, y, u, v):
        """Y (h, w) and U, V (h/2, w/2) numpy planes -> JPEG bytes."""
        import numpy as np
        if self.fmt != "yuv420":
            raise ValueError("this encoder takes R,G,B")
        mm, length = self.inbuf[0]
        self._sync(end=False)
        st, cs, rows = self.stride, self.stride // 2, self.rows
        h, w, h2, w2 = self.h, self.w, self.h // 2, self.w // 2
        buf = np.frombuffer(mm, np.uint8, st * rows * 3 // 2)
        buf[:st * rows].reshape(rows, st)[:h, :w] = y
        uo = st * rows
        vo = uo + cs * (rows // 2)
        buf[uo:vo].reshape(rows // 2, cs)[:h2, :w2] = u
        buf[vo:vo + cs * (rows // 2)].reshape(rows // 2, cs)[:h2, :w2] = v
        del buf                                               # (release the mmap view)
        self._sync(end=True)
        return self._run(length)

    def _run(self, length):
        """Hand the filled input buffer to the encoder and take the JPEG back."""
        v = self.v
        dma = self.dma_fd >= 0
        memory = v.V4L2_MEMORY_DMABUF if dma else v.V4L2_MEMORY_MMAP

        b = v.v4l2_buffer()
        planes = self._planes()
        b.type = v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
        b.memory = memory
        b.index = 0
        b.field = v.V4L2_FIELD_NONE
        b.length = 1
        b.m.planes = planes
        if dma:
            b.m.planes[0].m.fd = self.dma_fd
        b.m.planes[0].bytesused = min(self.in_size, length)
        b.m.planes[0].length = length
        self._io("give frame to encoder", v.VIDIOC_QBUF, b)

        self._wait(select.POLLIN)                             # compressed frame ready
        c = v.v4l2_buffer()
        planes = self._planes()
        c.type = v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        c.memory = v.V4L2_MEMORY_MMAP
        c.length = 1
        c.m.planes = planes
        self._io("take JPEG from encoder", v.VIDIOC_DQBUF, c)
        omm, olen = self.outbufs[c.index]
        jpeg = omm[:c.m.planes[0].bytesused]
        index = c.index
        c = v.v4l2_buffer()                                   # give the buffer back
        planes = self._planes()
        c.type = v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
        c.memory = v.V4L2_MEMORY_MMAP
        c.index = index
        c.length = 1
        c.m.planes = planes
        c.m.planes[0].bytesused = 0
        c.m.planes[0].length = olen
        self._io("return output buffer", v.VIDIOC_QBUF, c)

        self._wait(select.POLLOUT)                            # input buffer free again
        d = v.v4l2_buffer()
        planes = self._planes()
        d.type = v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
        d.memory = memory
        d.length = 1
        d.m.planes = planes
        self._io("take back input buffer", v.VIDIOC_DQBUF, d)
        return bytes(jpeg)

    def close(self):
        v = self.v
        for t in (v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE):
            try:
                fcntl.ioctl(self.vd, v.VIDIOC_STREAMOFF, v.v4l2_buf_type(t))
            except Exception:
                pass
        for mm, _ in self.inbuf + self.outbufs:
            try:
                mm.close()
            except Exception:
                pass
        for t, mem in ((v.V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                        v.V4L2_MEMORY_DMABUF if self.dma_fd >= 0 else v.V4L2_MEMORY_MMAP),
                       (v.V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, v.V4L2_MEMORY_MMAP)):
            try:
                req = v.v4l2_requestbuffers()
                req.count, req.type, req.memory = 0, t, mem
                fcntl.ioctl(self.vd, v.VIDIOC_REQBUFS, req)
            except Exception:
                pass
        if self.dma_fd >= 0:
            os.close(self.dma_fd)
            self.dma_fd = -1
        self.vd.close()


def _test_picture(w, h):
    import numpy as np
    x = (np.arange(w) * 255 // max(1, w - 1)).astype(np.uint8)
    y = (np.arange(h) * 255 // max(1, h - 1)).astype(np.uint8)
    t = np.empty((h, w, 3), np.uint8)
    t[..., 0] = x[None, :]
    t[..., 1] = y[:, None]
    t[..., 2] = 96
    return t


def _test_planes(w, h):
    """The test picture's Y, U, V planes (U, V half size), worked out with whole
    numbers straight from its gradients - no full-size colour conversion."""
    import numpy as np
    x = np.arange(w) * 255 // max(1, w - 1)
    y = np.arange(h) * 255 // max(1, h - 1)
    # Y = 0.299 R + 0.587 G + 0.114 B  (R = x, G = y, B = 96), in 1/256 steps
    yx = (77 * x).astype(np.uint16)
    yy = (150 * y + 29 * 96 + 128).astype(np.uint16)
    Y = ((yy[:, None] + yx[None, :]) >> 8).astype(np.uint8)
    xs, ys = x[::2], y[::2]
    U = np.clip(128 + (-43 * xs[None, :] - 85 * ys[:, None] + 128 * 96 + 128) // 256, 0, 255)
    V = np.clip(128 + (128 * xs[None, :] - 107 * ys[:, None] - 21 * 96 + 128) // 256, 0, 255)
    return Y, U.astype(np.uint8)[:h // 2, :w // 2], V.astype(np.uint8)[:h // 2, :w // 2]


def _test_small(w, h, sw, sh):
    """The test picture's block averages, at the size the decoder gave (sw x sh,
    normally 1/8: then each pixel is an 8x8 block's average)."""
    import numpy as np
    x = (np.arange(w) * 255 // max(1, w - 1)).astype(np.float32)
    y = (np.arange(h) * 255 // max(1, h - 1)).astype(np.float32)
    f = max(1, -(-w // sw))
    cx = np.add.reduceat(x, np.arange(0, w, f)) / np.diff(np.append(np.arange(0, w, f), w))
    cy = np.add.reduceat(y, np.arange(0, h, f)) / np.diff(np.append(np.arange(0, h, f), h))
    out = np.empty((sh, sw, 3), np.float32)
    out[..., 0] = cx[None, :sw]
    out[..., 1] = cy[:sh, None]
    out[..., 2] = 96
    return out


def checked(width, height, bits=2.0, fmt="rgb24"):
    """A HwJpeg if the hardware encoder is there and gives a correct picture;
    otherwise None (Zero Ichi then compresses in software). Each size and format
    is tested with a test picture once; after that it opens straight away."""
    import io
    import numpy as np
    from PIL import Image
    key = (width, height, fmt)
    if key in _VERIFIED:
        try:
            return HwJpeg(width, height, mode=_VERIFIED[key], bits=bits, fmt=fmt)
        except Exception as e:
            log.info("Hardware JPEG could not open again (%s) - compressing in software", e)
            return None
    enc, errors = None, []
    for mode in ("mmap", "dmabuf"):
        try:
            enc = HwJpeg(width, height, mode=mode, bits=bits, fmt=fmt)
            break
        except Exception as e:
            errors.append(f"{mode}: {e}")
    if enc is None:
        log.info("Hardware JPEG not available for %dx%d %s (%s) - compressing in software",
                 width, height, fmt, "; ".join(errors))
        return None
    try:
        if fmt == "yuv420":
            data = enc.encode_yuv(*_test_planes(width, height))
        else:
            data = enc.encode(_test_picture(width, height))
        # Check it at 1/8 size: the JPEG decoder then only reads each 8x8 block's
        # average - many times quicker than decoding the whole picture, and the
        # gradients of the test picture show any mix-up just as well.
        im = Image.open(io.BytesIO(data))
        if im.size != (width, height):
            raise RuntimeError(f"test picture came out {im.size[0]}x{im.size[1]}")
        im.draft("RGB", (max(1, width // 8), max(1, height // 8)))
        got = np.asarray(im.convert("RGB"), np.float32)
        want = _test_small(width, height, got.shape[1], got.shape[0])
        diff = float(np.abs(got - want).mean())
        if diff > 12:
            raise RuntimeError(f"test picture came out wrong (diff {diff:.0f})")
        _VERIFIED[key] = enc.mode
        if not _LOGGED.get((enc.mode, fmt)):              # say it once, not every time
            log.info("Hardware JPEG in use (%s buffers, %s input)", enc.mode, fmt)
            _LOGGED[(enc.mode, fmt)] = True
        return enc
    except Exception as e:
        log.info("Hardware JPEG not used for %dx%d %s (%s) - compressing in software",
                 width, height, fmt, e)
        enc.close()
        return None
