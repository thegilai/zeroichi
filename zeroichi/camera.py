"""
Camera access for Zero Ichi (Picamera2), plus a background loader.

Starting the camera is the slowest part of Zero Ichi's start-up on a Pi Zero, so:
  * CameraLoader does it in a background thread while the rest starts up
  * Picamera2 is loaded first, while the camera's devices are still being set
    up by the system, and parts of it Zero Ichi never uses are skipped
  * then it waits only for the camera's own devices (not all hardware)
numpy is imported inside functions so this module itself loads instantly.
"""
import glob
import logging
import math
import os
import subprocess
import sys
import threading
import time
import types

PREVIEW_SIZE = (320, 240)      # sensor-friendly 4:3
PHOTO_SIZE = (1600, 1200)      # safe on 512 MB RAM with any Pi camera module
# When the camera is turned 90/270 degrees we still want a landscape picture, so
# it's cut from the middle of the sideways image - which needs more pixels:
PREVIEW_SIZE_SIDEWAYS = (432, 320)   # -> 320x432 after turning -> exactly 320x240
PHOTO_SIZE_SIDEWAYS = (2048, 1536)   # -> 1536x2048 after turning -> 1600x1200
# In PHOTO mode the camera runs one setup with two streams: this one for photos
# (the sensor's 2x2-binned mode: whole picture, low noise, light on memory) and
# a small one for the live view. A photo is then just a copy of the current
# frame - same exposure as on screen, no mode switch, no waiting.
PHOTO_STREAM = (1296, 972)
# The camera delivers frames at 15 per second for the live view (it would
# otherwise run at 30-58, and handling every frame costs the Pi Zero CPU time
# even though the screen only shows a few of them).
LIVE_FPS = 15
_FRAME_US = int(1e6 / LIVE_FPS)
# Frames dropped after switching to still mode before the photo is taken (the
# exposure settles); the second, longer one is used if the photo came out white.
STILL_SETTLE_FRAMES = (3, 10)

log = logging.getLogger("zeroichi")

# Parts of Picamera2 that load at start-up but that Zero Ichi never uses (HDMI
# preview, DNG/EXIF/JPEG saving - Zero Ichi saves with Pillow). Skipping them makes
# the camera start sooner. If that ever fails, Picamera2 is loaded normally.
# (Not simplejpeg: Zero Ichi itself uses it for video, and the app loads at the same
#  time in another thread - it must never see a placeholder.)
_UNUSED = ("kms", "pykms", "pidng", "pidng.core", "pidng.camdefs", "piexif")


class _Anything:
    """Stands in for anything picamera2 looks up in a skipped module while it
    loads (e.g. kms.PixelFormats.RGB888) - never actually used by Zero Ichi."""
    def __init__(self, name):
        self._name = name

    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        return _Anything(f"{self._name}.{name}")

    def __call__(self, *a, **k):
        return _Anything(self._name + "()")

    def __getitem__(self, key):
        return _Anything(f"{self._name}[]")

    def __iter__(self):
        return iter(())

    def __mro_entries__(self, bases):            # allow "class X(skipped.Thing)"
        return (object,)

    def __hash__(self):
        return hash(self._name)

    def __eq__(self, other):
        return isinstance(other, _Anything) and other._name == self._name


class _Unused(types.ModuleType):
    def __getattr__(self, name):
        if name.startswith("__"):
            raise AttributeError(name)
        return _Anything(f"{self.__name__}.{name}")


def _import_picamera2():
    added = []
    for name in _UNUSED:
        if name not in sys.modules:
            sys.modules[name] = _Unused(name)
            added.append(name)
    try:
        from picamera2 import Picamera2
        return Picamera2
    except Exception as e:
        log.info("Quick camera load failed (%s) - loading normally", e)
        for name in list(sys.modules):
            if name == "picamera2" or name.startswith("picamera2."):
                del sys.modules[name]
        for name in added:
            sys.modules.pop(name, None)
        added = []
        from picamera2 import Picamera2
        return Picamera2
    finally:
        for name in added:
            sys.modules.pop(name, None)


def _device_name(node):
    """Name the kernel gives a camera device, e.g. 'unicam-image', 'bcm2835-isp',
    'ov5647 10-0036' or 'bcm2835-codec-decode'."""
    base = os.path.basename(node)
    for p in (f"/sys/class/video4linux/{base}/name", f"/sys/bus/media/devices/{base}/model"):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            continue
    return ""


def _register_devices():
    """Fast start: the device manager (udev) doesn't run, so register the video
    devices the way it would (an entry in its database) - libcamera only uses
    registered devices (see fast-start.sh: register_devices)."""
    if (os.environ.get("ZEROICHI_FASTINIT") != "1" or os.path.exists("/run/udev/control")
            or os.geteuid() != 0):
        return
    for d in glob.glob("/sys/class/video4linux/*") + glob.glob("/sys/bus/media/devices/*"):
        try:
            with open(d + "/dev") as f:
                mm = f.read().strip()
            p = f"/run/udev/data/c{mm}"
            if not os.path.exists(p):
                os.makedirs("/run/udev/data", exist_ok=True)
                with open(p, "w") as f:
                    f.write(f"I:{int(time.monotonic() * 1e6)}\n")
        except OSError:
            continue


def _wait_for_camera_devices(timeout=8.0):
    """Early in the boot the camera's devices may still be being set up, and
    libcamera only sees devices the system (udev) has finished with. Wait for
    the ones the camera needs - not the hardware video encoder/decoder (codec),
    which Zero Ichi doesn't use and which can be slow to appear. Then the camera
    is simply retried, so this wait is capped."""
    end = time.monotonic() + timeout
    stable, last, missing = 0, None, []
    while time.monotonic() < end:
        _register_devices()
        nodes = sorted(n for n in glob.glob("/dev/media*") + glob.glob("/dev/video*")
                       + glob.glob("/dev/v4l-subdev*") if "codec" not in _device_name(n))
        missing = []
        for n in nodes:
            try:
                rdev = os.stat(n).st_rdev
            except OSError:
                missing.append(n)
                continue
            if not os.path.exists(f"/run/udev/data/c{os.major(rdev)}:{os.minor(rdev)}"):
                missing.append(n)
        names = [_device_name(n) for n in nodes]
        has_unicam = any("unicam" in n for n in names)
        has_sensor = any(n and not n.startswith(("unicam", "bcm2835")) for n in names
                         if n)                                    # e.g. "ov5647 10-0036"
        has_isp = any(n.startswith("bcm2835-isp") for n in names if n)
        ok = bool(nodes) and not missing and has_unicam and has_isp and has_sensor
        stable = stable + 1 if (ok and nodes == last) else 0
        last = nodes
        if stable >= 2:
            return True
        time.sleep(0.1)
    log.info("Camera devices still being set up after %.0f s (%s) - trying anyway",
             timeout, ", ".join(f"{m} {_device_name(m)}".strip() for m in missing[:4]) or "none found")
    return False


class Camera:
    def __init__(self, fake=False, swap_rb=False, preview_size=PREVIEW_SIZE, lores=None,
                 main_yuv=False, video_yuv=False,
                 hflip=False, vflip=False):
        self.fake = fake
        self.swap_rb = swap_rb
        self.preview_size = preview_size
        self.hflip, self.vflip = hflip, vflip
        self.lores = tuple(lores) if lores else None   # small second stream
        self.main_yuv = bool(main_yuv) and bool(lores)  # main stream = photo stream (YUV)
        # VIDEO mode: the recording stream is YUV too - half the data of RGB for
        # the camera to write and the video encoder to read (JPEG stores YUV anyway)
        self.video_yuv = bool(video_yuv) and bool(lores) and not self.main_yuv
        self._fallback_size = None
        self.hw_flip = True               # flips done by the sensor (free)
        self._stills = {}
        self.t0 = time.monotonic()
        self.timing = {}
        self._tap, self._tap_broken, self._tap_misses = None, False, 0
        if fake:
            log.info("Using fake camera (test pattern)")
            return
        t = time.monotonic()
        try:
            import libcamera  # noqa: F401  (the camera library itself - timed on its own)
        except ImportError:
            pass
        self.timing["libcamera"] = time.monotonic() - t
        t = time.monotonic()
        Picamera2 = _import_picamera2()
        self.timing["picamera2"] = time.monotonic() - t
        t = time.monotonic()
        _wait_for_camera_devices()
        self.timing["devices"] = time.monotonic() - t
        t = time.monotonic()
        deadline = time.monotonic() + 30
        tries = 0
        while True:                       # the camera appears as soon as it's ready
            try:
                self.cam = Picamera2()
                break
            except Exception as e:
                tries += 1
                if time.monotonic() > deadline:
                    raise
                if tries in (1, 5) or tries % 10 == 0:
                    log.info("Camera not ready yet (%s) - retrying", e)
                # picamera2 keeps libcamera's first camera list; if that was made
                # while the camera was still being set up, it would never change.
                # reset() makes the next try look again from scratch.
                try:
                    Picamera2._cm.reset()
                except Exception:
                    pass
                time.sleep(0.5)
        self.timing["tries"] = float(tries)
        self.timing["open"] = time.monotonic() - t
        self._tap, self._tap_broken, self._tap_misses = _LoresTap(), False, 0
        try:
            self.cam.post_callback = self._tap.callback
        except Exception:
            self._tap = None
        t = time.monotonic()
        self._start_preview()
        self.timing["start"] = time.monotonic() - t

    def _transform(self):
        if not self.hw_flip:
            return None
        try:
            from libcamera import Transform
            return Transform(hflip=int(self.hflip), vflip=int(self.vflip))
        except Exception:
            self.hw_flip = False
            return None

    def _start_preview(self):
        # NB: in Picamera2, "BGR888" means the array comes out in R,G,B order.
        kw = {}
        tr = self._transform()
        if tr is not None:
            kw["transform"] = tr
        buffers = 4
        if self.lores:                       # small second stream for the screen
            kw["lores"] = {"size": self.lores, "format": "YUV420"}
            if not self.main_yuv:
                buffers = 6                  # recording: the encoder holds some (like picamera2)
        fmt = "YUV420" if (self.main_yuv or self.video_yuv) else "BGR888"
        try:
            self.preview_cfg = self.cam.create_preview_configuration(
                main={"size": self.preview_size, "format": fmt}, buffer_count=buffers,
                controls={"FrameDurationLimits": (_FRAME_US, _FRAME_US), **self._kept()}, **kw)
            self.cam.configure(self.preview_cfg)
        except Exception as e:
            if not kw:
                raise
            log.info("Camera setup failed (%s) - trying without flips / small stream", e)
            self.hw_flip = False
            self.lores = None
            self.video_yuv = False
            if self.main_yuv:                # no photo stream: photos switch modes again
                self.main_yuv = False
                self.preview_size = self._fallback_size or self.preview_size
            self.preview_cfg = self.cam.create_preview_configuration(
                main={"size": self.preview_size, "format": "BGR888"}, buffer_count=4,
                controls={"FrameDurationLimits": (_FRAME_US, _FRAME_US), **self._kept()})
            self.cam.configure(self.preview_cfg)
        self.cam.start()

    def reconfigure(self, preview_size=None, hflip=None, vflip=None, lores=False,
                    main_yuv=False, fallback_size=None, video_yuv=False):
        """Change the stream size, sensor flips, and the optional small second
        stream ("lores": for the screen while the big one is recorded, or while
        the big one is the photo stream - then main_yuv=True)."""
        size = preview_size or self.preview_size
        h = self.hflip if hflip is None else hflip
        v = self.vflip if vflip is None else vflip
        lo = None if lores in (False, None) else tuple(lores)
        vy = bool(video_yuv) and lo is not None and not main_yuv
        key = (size, h, v, lo, bool(main_yuv), vy)
        if key == (self.preview_size, self.hflip, self.vflip, self.lores, self.main_yuv,
                   self.video_yuv):
            return
        self.preview_size, self.hflip, self.vflip, self.lores = size, h, v, lo
        self.main_yuv = bool(main_yuv)
        self.video_yuv = vy
        self._fallback_size = fallback_size
        self._stills.clear()
        if not self.fake:
            self.cam.stop()
            if self._tap is not None:
                self._tap.clear()                    # (frames of the old setup)
            self._start_preview()
            if self.paused:                          # (screen saver etc.: stay stopped)
                self.cam.stop()

    def set_preview_size(self, size):
        self.reconfigure(preview_size=size)

    def _fix(self, a):
        import numpy as np
        return np.ascontiguousarray(a[..., ::-1]) if self.swap_rb else a

    def _pattern(self, w, h):
        import numpy as np
        t = time.monotonic() - self.t0
        x = np.arange(w)[None, :]
        y = np.arange(h)[:, None]
        img = np.empty((h, w, 3), np.uint8)
        img[..., 0] = (x * 255 // w + int(t * 40)) % 256
        img[..., 1] = y * 255 // h
        img[..., 2] = int(128 + 100 * math.sin(t))
        cx = int(w / 2 + w / 4 * math.cos(t))
        cy = int(h / 2 + h / 4 * math.sin(t * 1.3))
        img[(x - cx) ** 2 + (y - cy) ** 2 < (h // 6) ** 2] = (255, 255, 255)
        img[: h // 8, : h // 8] = (255, 0, 0)         # red = top-left (orientation)
        if self.hflip:                                 # behave like the sensor would
            img = img[:, ::-1]
        if self.vflip:
            img = img[::-1]
        return np.ascontiguousarray(img)

    def frame(self, gray=False):
        """The next live-view frame (R,G,B numpy array): from the small second
        stream when there is one, else from the main stream. gray=True gives
        just the brightness (2-D array) - for black & white looks, which then
        skip all colour work (the camera's YUV already has it as its own plane)."""
        import numpy as np
        if self.fake:
            time.sleep(0.05)
            f = self._pattern(*(self.lores or self.preview_size))
            return np.ascontiguousarray(f[..., 1]) if gray else f
        if self.lores:
            a = self._lores_frame()
            w, h = self.lores
            if gray:
                return a[:h, :w]                       # the Y plane (array is a fresh copy)
            return self._fix(yuv420_to_rgb(a, w, h))
        f = self._fix(self.cam.capture_array())
        if gray:
            from PIL import Image
            return np.asarray(Image.fromarray(f).convert("L"))
        return f

    def frame_yuv(self):
        """The next live-view frame as the camera's own (Y, U, V) planes (U, V at
        half size) - for looks that work in YUV (16-Bit): no colour conversion.
        Without the small stream: an R,G,B array instead."""
        if self.fake:
            time.sleep(0.05)
            w, h = self.lores or self.preview_size
            return _planes(rgb_to_yuv420(self._pattern(w, h)), w, h)
        if not self.lores:
            return self._fix(self.cam.capture_array())
        w, h = self.lores
        a = self._lores_frame()                          # (a fresh copy)
        Y, U, V = _planes(a, w, h)
        return (Y, V, U) if self.swap_rb else (Y, U, V)

    def _lores_frame(self):
        """The newest small-stream frame (YUV420 array). picamera2 calls our tap
        for every frame in its own camera thread, where the frame is copied;
        here we just pick it up - instead of asking picamera2 for a frame each
        time (a job handed to its thread and back: a few ms per frame on a Pi
        Zero). Falls back to asking if the tap doesn't deliver."""
        w, h = self.lores
        tap = self._tap
        if tap is not None and not self._tap_broken:
            a = tap.get(0.5)
            if a is not None and a.shape[0] == h * 3 // 2 and a.shape[1] >= w:
                self._tap_misses = 0
                return a
            self._tap_misses += 1
            if self._tap_misses >= 5:                  # it doesn't work here: stop using it
                self._tap_broken = True
                log.info("Frame tap not delivering - asking picamera2 for each frame")
        return self.cam.capture_array("lores")

    def frame_pair(self):
        """(live-view frame, full main-stream frame) from ONE camera frame - for
        GIF recording: the GIF keeps the full frame, the screen shows the small."""
        if self.fake:
            time.sleep(0.05)
            return (self._pattern(*(self.lores or self.preview_size)),
                    self._pattern(*self.preview_size))
        if not self.lores:
            f = self._fix(self.cam.capture_array())
            return f, f
        req = self.cam.capture_request()
        try:
            lo = yuv420_to_rgb(req.make_array("lores"), *self.lores)
            main = req.make_array("main")
        finally:
            req.release()
        if self.main_yuv or self.video_yuv:              # (normally RGB when this is used)
            main = yuv420_to_rgb(main, *self.preview_size)
        return self._fix(lo), self._fix(main)

    @property
    def photo_stream(self):
        """True when photos can be copied straight from the running main stream."""
        return self.main_yuv and bool(self.lores)

    def capture_still(self):
        """A photo from the photo stream: a copy of the current frame (taken at
        the exposure the live view has settled on). Colour conversion happens
        later, in the background (YuvFrame.rgb)."""
        if self.fake:                                   # simulator: same YUV layout
            return YuvFrame(rgb_to_yuv420(self._pattern(*PHOTO_STREAM)), *PHOTO_STREAM)
        w, h = self.preview_size
        return YuvFrame(self.cam.capture_array("main"), w, h, self.swap_rb)

    def capture(self, size=PHOTO_SIZE):
        """Take a full-size photo. The camera switches to its still mode for
        this; the first frames after the switch can be badly exposed (white),
        so a few are dropped first, and a white or black result is retaken."""
        if self.fake:
            return self._pattern(*size)
        cfg = self._stills.get(size)
        if cfg is None:
            kw = {}
            tr = self._transform()
            if tr is not None:
                kw["transform"] = tr
            cfg = self._stills[size] = self.cam.create_still_configuration(
                main={"size": size, "format": "BGR888"}, buffer_count=1,
                # picamera2 allows exposures up to 1000 s by default; 0.2 s at most
                controls={"FrameDurationLimits": (33333, 200000)}, **kw)
        arr = None
        for delay in STILL_SETTLE_FRAMES:
            arr, md = self._still(cfg, delay)
            level = float(arr[::16, ::16].mean())
            log.info("Photo: exposure %.1f ms, gain %.2f, brightness %.0f (after %d frames)",
                     md.get("ExposureTime", 0) / 1000, md.get("AnalogueGain", 0), level, delay)
            if 6 < level < 245:
                break
            log.warning("Photo came out %s - taking it again", "white" if level >= 245 else "black")
        return self._fix(arr)

    def _still(self, cfg, delay):
        """One still frame (+ its exposure info) after dropping `delay` frames."""
        try:
            req = self.cam.switch_mode_and_capture_request(cfg, delay=delay)
        except TypeError:                     # older picamera2 without "delay"
            req = self.cam.switch_mode_and_capture_request(cfg)
        try:
            return req.make_array("main"), (req.get_metadata() or {})
        finally:
            req.release()

    # -- hardware video recording ------------------------------------------
    def start_video(self, writer, fps=15):
        """Record the camera stream with the Pi's hardware MJPEG encoder: each
        compressed frame goes straight into `writer` (a MovWriter), at a steady
        frame rate, using almost no CPU. Raises if there's no hardware encoder."""
        from movie import huffman_parts
        if self.fake:                                  # simulator: a stand-in encoder
            self._fake_stop = threading.Event()

            def run():
                t0 = time.monotonic()
                while not self._fake_stop.is_set():
                    writer.add_frame(self._pattern(*self.preview_size), time.monotonic() - t0)
                    self._fake_stop.wait(1 / fps)
            self._fake_rec = threading.Thread(target=run, daemon=True)
            self._fake_rec.start()
            return
        from picamera2.encoders import MJPEGEncoder, Quality
        from picamera2.outputs import Output

        class ToMov(Output):
            def __init__(self):
                super().__init__()
                self.t0 = None

            def outputframe(self, frame, keyframe=True, timestamp=None, packet=None,
                            audio=False):
                if audio:
                    return
                t = (timestamp or 0) / 1e6
                if self.t0 is None:
                    self.t0 = t
                writer.add_jpeg_parts(huffman_parts(frame), t - self.t0)

        us = int(1e6 / fps)
        self.cam.set_controls({"FrameDurationLimits": (us, us)})
        self._encoder = MJPEGEncoder()
        try:
            self.cam.start_encoder(self._encoder, ToMov(), name="main", quality=Quality.HIGH)
        except Exception:
            self._abandon_encoder()
            raise

    def _abandon_encoder(self):
        """An encoder that failed to start: make sure its helper thread ends and
        its device is closed (picamera2 may have started them already)."""
        enc, self._encoder = self._encoder, None
        try:
            enc._running = False
            th = getattr(enc, "thread", None)
            if th is not None and th.is_alive():
                th.join(1.0)
            vd = getattr(enc, "vd", None)
            if vd is not None and not vd.closed:
                vd.close()
        except Exception:
            pass
        try:
            self.cam.set_controls({"FrameDurationLimits": (_FRAME_US, _FRAME_US)})
        except Exception:
            pass

    def stop_video(self):
        if self.fake:
            self._fake_stop.set()
            self._fake_rec.join(timeout=2)
            return
        try:
            self.cam.stop_encoder()
        finally:
            self._encoder = None
            try:
                self.cam.set_controls({"FrameDurationLimits": (_FRAME_US, _FRAME_US)})
            except Exception:
                pass

    def pause(self):
        """Stop the camera for a while (no frames, no CPU used). Counted: the
        screen saver and the developing screen can both pause it, and it only
        runs again when both are done."""
        self._pauses = getattr(self, "_pauses", 0) + 1
        if self._pauses == 1 and not self.fake:
            self.cam.stop()

    def resume(self):
        if getattr(self, "_pauses", 0) <= 0:
            return
        self._pauses -= 1
        if self._pauses == 0 and not self.fake:
            self.cam.start()

    @property
    def paused(self):
        return getattr(self, "_pauses", 0) > 0

    def _kept(self):
        """(v54) EV: set again with each new set-up (a new configuration
        starts from the camera's defaults)"""
        return dict(getattr(self, "_ctl", {}))

    def set_ev(self, ev):
        """(0.1.0) EV: given at a quick restart of the camera, so it shows at
        once - sent with the next pictures, the camera library held it back
        after the camera's first restart (see camd.Server.set_ev). Not while a
        video is being recorded: then with the next pictures."""
        if self.fake:
            return
        if getattr(self, "_ctl", {}).get("ExposureValue") == ev:
            return
        self._ctl = dict(getattr(self, "_ctl", {}), ExposureValue=ev)
        try:
            restart = (getattr(self.cam, "started", False) and not self.paused
                       and getattr(self, "_encoder", None) is None)
            if restart:
                self.cam.stop()
            self.cam.set_controls({"ExposureValue": ev})     # (stopped: used at the start)
            if restart:
                self.cam.start()
        except Exception as e:
            log.warning("Could not set EV: %s", e)

    def close(self):
        if not self.fake:
            try:
                self.cam.stop()
                self.cam.close()
            except Exception:
                pass


# GIF / video frame sizes per "Clip size" (4:3, square) - all multiples of 16,
# which the hardware video encoder handles best.
# (v55: just two - the older sizes become the nearer of them)
CLIP_SIZES = {"320x240": ((320, 240), (240, 240)),
              "640x480": ((640, 480), (480, 480))}
OLD_CLIP_NAMES = {"Small": "320x240", "Medium": "640x480", "Large": "640x480",
                  "384x288": "320x240", "448x336": "320x240", "512x384": "640x480", "576x432": "640x480"}
SQUARE_FILTER_NAMES = {"Holga B&W", "Holga Color"}   # always square (Holga = 6x6 film)


def picture_aspect(state, screen_w, screen_h):
    """Width/height of pictures for these settings (shared by launcher + app)."""
    if state.get("filter_name") in SQUARE_FILTER_NAMES or state.get("size") == "Square":
        return 1.0
    if state.get("size") == "4:3":
        return 4 / 3
    return screen_w / screen_h


def target_size_for(state, screen_w, screen_h, kind):
    """Pixel size of a "photo", a GIF/video frame ("clip") or the live "view"."""
    a = picture_aspect(state, screen_w, screen_h)
    square = a == 1.0
    if kind == "photo":
        # the largest picture of this shape that fits in the photo stream
        rot = state.get("cam_rotate") if state.get("cam_rotate") in (0, 90, 180, 270) else 0
        turn = flips_for(rot, bool(state.get("mirror")))[2]
        aw, ah = (PHOTO_STREAM[1], PHOTO_STREAM[0]) if turn else PHOTO_STREAM
        if aw / ah >= a:
            ph, pw = ah, int(round(ah * a))
        else:
            pw, ph = aw, int(round(aw / a))
        return (pw - pw % 2, ph - ph % 2)
    if kind == "clip":
        key = OLD_CLIP_NAMES.get(state.get("clip_size"), state.get("clip_size"))
        sizes = CLIP_SIZES.get(key, CLIP_SIZES["640x480"])
        return sizes[1 if square else 0]
    if a >= screen_w / screen_h:                         # fit to the screen
        w, h = screen_w, int(round(screen_w / a))
    else:
        w, h = int(round(screen_h * a)), screen_h
    # half size, shown doubled: ~4x less work for every frame
    return (max(2, w // 2 // 2 * 2), max(2, h // 2 // 2 * 2))


def stream_size_for(state, screen_w, screen_h, recording=False):
    """Camera stream size: just big enough for the live view, or for the clip
    size while recording a GIF / video."""
    rot = state.get("cam_rotate") if state.get("cam_rotate") in (0, 90, 180, 270) else 0
    turn = flips_for(rot, bool(state.get("mirror")))[2]
    target = target_size_for(state, screen_w, screen_h, "clip" if recording else "view")
    return sensor_size_for(target, turn)


def stream_plan(state, screen_w, screen_h):
    """The camera setup for each mode - kept running, so the exposure the live
    view settles on is exactly what photos and recordings get (switching the
    camera to another setup restarts its auto-exposure).
      PHOTO:       photo stream (YUV) + small live-view stream
      GIF / VIDEO: recording stream (clip size) + small live-view stream:
                   pressing record just starts recording it.
    Returns (main size, small stream size, main is the photo stream)."""
    if state.get("mode", "PHOTO") == "PHOTO":
        return PHOTO_STREAM, stream_size_for(state, screen_w, screen_h), True
    rot = state.get("cam_rotate") if state.get("cam_rotate") in (0, 90, 180, 270) else 0
    turn = flips_for(rot, bool(state.get("mirror")))[2]
    cw, ch = target_size_for(state, screen_w, screen_h, "clip")
    vw, vh = target_size_for(state, screen_w, screen_h, "view")
    # (both streams have the picture's shape: the small one is scaled from the
    #  same part of the sensor, so a different shape would come out stretched)
    if turn:
        return (ch, cw), (vh, vw), False
    return (cw, ch), (vw, vh), False


def sensor_size_for(target, turn):
    """Smallest 4:3 camera stream that still gives `target` (w, h) after the
    optional 90-degree turn and the crop to the picture's shape."""
    tw, th = target
    if turn:                              # the stream's height becomes the width
        sh = max(tw, -(-th * 3 // 4))
    else:
        sh = max(th, -(-tw * 3 // 4))
    sh += sh % 2
    sw = -(-(sh * 4 // 3) // 16) * 16     # width: multiple of 16
    return (sw, sh)


class NoCamera:
    """Used while the real camera can't be found: a plain dark picture (with a
    message drawn on top by the app) - never a fake image, and no photos."""
    fake = False
    missing = True
    hw_flip = True

    def __init__(self, preview_size=PREVIEW_SIZE):
        self.preview_size = preview_size
        self.hflip = self.vflip = False
        self.timing = {}

    def frame(self, gray=False):
        import numpy as np
        time.sleep(0.1)
        w, h = self.preview_size
        return np.full((h, w) if gray else (h, w, 3), 24, np.uint8)

    def capture(self, size=PHOTO_SIZE):
        raise RuntimeError("no camera")

    photo_stream = False
    lores = None

    def frame_pair(self):
        f = self.frame()
        return f, f

    def reconfigure(self, preview_size=None, hflip=None, vflip=None, lores=False, **kw):
        if preview_size:
            self.preview_size = preview_size

    def set_preview_size(self, size):
        self.preview_size = size

    def set_ev(self, ev):
        pass

    def set_wb(self, name):
        pass

    def close(self):
        pass


class YuvFrame:
    """A YUV420 frame copied from the camera. .planes() crops and turns it
    without any colour conversion (photos are saved from the planes directly);
    .rgb() converts the whole frame (slow at full size on a Pi Zero)."""

    def __init__(self, array, w, h, swap_rb=False):
        self.array, self.w, self.h, self.swap_rb = array, w, h, swap_rb

    def planes(self, out_size, turn=False, hflip=False, vflip=False):
        """(Y, U, V) numpy planes of the centred part that becomes a photo of
        out_size (w, h) - turned 90 degrees clockwise if `turn`. U and V are
        half size. All crop edges are even, so the colour planes line up."""
        import numpy as np
        a, w, h = self.array, self.w, self.h
        st = a.shape[1]
        flat = a.reshape(-1)
        cs, ch, cw = st // 2, h // 2, w // 2
        Y = flat[:h * st].reshape(h, st)[:, :w]
        U = flat[h * st:h * st + ch * cs].reshape(ch, cs)[:, :cw]
        V = flat[h * st + ch * cs:h * st + 2 * ch * cs].reshape(ch, cs)[:, :cw]
        tw, th = out_size
        sw, sh = (th, tw) if turn else (tw, th)
        sw, sh = min(sw, w) & ~1, min(sh, h) & ~1
        x0, y0 = ((w - sw) // 2) & ~1, ((h - sh) // 2) & ~1
        out = [Y[y0:y0 + sh, x0:x0 + sw],
               U[y0 // 2:(y0 + sh) // 2, x0 // 2:(x0 + sw) // 2],
               V[y0 // 2:(y0 + sh) // 2, x0 // 2:(x0 + sw) // 2]]
        for i, p in enumerate(out):
            if hflip:
                p = p[:, ::-1]
            if vflip:
                p = p[::-1]
            if turn:
                p = np.rot90(p, -1)                    # 90 degrees clockwise
            out[i] = np.ascontiguousarray(p)
        return out

    def rgb(self):
        import numpy as np
        a = yuv420_to_rgb(self.array, self.w, self.h, smooth=True)
        return np.ascontiguousarray(a[..., ::-1]) if self.swap_rb else a


def _planes(a, w, h):
    """picamera2's YUV420 array -> (Y, U, V) views (rows padded to the stride)."""
    st = a.shape[1]
    flat = a.reshape(-1)
    cs, ch, cw = st // 2, h // 2, w // 2
    return (flat[:h * st].reshape(h, st)[:, :w],
            flat[h * st:h * st + ch * cs].reshape(ch, cs)[:, :cw],
            flat[h * st + ch * cs:h * st + 2 * ch * cs].reshape(ch, cs)[:, :cw])


def rgb_to_yuv420(rgb):
    """R,G,B array -> picamera2's YUV420 array layout (for the simulator)."""
    import numpy as np
    from PIL import Image
    h, w = rgb.shape[:2]
    y, u, v = Image.fromarray(np.ascontiguousarray(rgb)).convert("YCbCr").split()
    st = (w + 63) // 64 * 64
    out = np.zeros((h * 3 // 2, st), np.uint8)
    flat = out.reshape(-1)
    flat[:h * st].reshape(h, st)[:, :w] = np.asarray(y)
    cs = st // 2
    flat[h * st:h * st + (h // 2) * cs].reshape(h // 2, cs)[:, :w // 2] = np.asarray(u.reduce(2))
    flat[h * st + (h // 2) * cs:h * st + h * cs].reshape(h // 2, cs)[:, :w // 2] = np.asarray(v.reduce(2))
    return out


def yuv420_to_rgb(a, w, h, smooth=False):
    """picamera2's YUV420 array (brightness plane, then quarter-size U and V
    planes, rows padded to the stride) -> R,G,B numpy array, using Pillow's C
    code: enlarge the colour planes, merge, convert."""
    import numpy as np
    from PIL import Image
    stride = a.shape[1]
    flat = a.reshape(-1)
    y = flat[:h * stride].reshape(h, stride)[:, :w]
    cs, ch, cw = stride // 2, h // 2, w // 2
    u = flat[h * stride:h * stride + ch * cs].reshape(ch, cs)[:, :cw]
    v = flat[h * stride + ch * cs:h * stride + 2 * ch * cs].reshape(ch, cs)[:, :cw]
    planes = [Image.fromarray(np.ascontiguousarray(y))]
    for c in (u, v):
        planes.append(Image.fromarray(np.ascontiguousarray(c)).resize(
            (w, h), Image.BILINEAR if smooth else Image.NEAREST))
    return np.asarray(Image.merge("YCbCr", planes).convert("RGB"))


def flips_for(rotation, mirror):
    """How to get a picture turned `rotation` degrees clockwise (then mirrored):
    the sensor flips first (free), then software turns it 90 degrees if needed.
    Returns (hflip, vflip, turn_90)."""
    k = (rotation // 90) % 4
    h = v = k >= 2                       # 180 = flip both ways
    if mirror:                           # mirroring after a 90 turn = flipping
        if k % 2 == 0:                   # vertically before it
            h = not h
        else:
            v = not v
    return h, v, k % 2 == 1


class _LoresTap:
    """Receives every small-stream frame from picamera2's camera thread (its
    post_callback) and keeps the newest copy for the live view. It only copies
    while the live view is asking for frames."""

    def __init__(self):
        self.cond = threading.Condition()
        self.frame = None
        self.fresh = False
        self.want_until = 0.0

    def callback(self, request):
        if time.monotonic() > self.want_until:          # nobody watching (gallery, menu...)
            return
        try:
            a = request.make_array("lores")
        except Exception:
            return
        with self.cond:
            self.frame, self.fresh = a, True
            self.cond.notify_all()

    def get(self, timeout):
        """The newest frame not handed out yet (waits for one), or None."""
        self.want_until = time.monotonic() + 1.0
        with self.cond:
            if not self.fresh:
                self.cond.wait(timeout)
            if not self.fresh:
                return None
            self.fresh = False
            return self.frame

    def clear(self):
        with self.cond:
            self.frame, self.fresh = None, False


class CameraLoader(threading.Thread):
    """Starts the camera in the background. Call result() to get it (waits)."""

    def __init__(self, fake=False, swap_rb=False, preview_size=PREVIEW_SIZE,
                 hflip=False, vflip=False, lores=None, main_yuv=False, video_yuv=False):
        super().__init__(daemon=True, name="camera-loader")
        self.args = dict(fake=fake, swap_rb=swap_rb, preview_size=preview_size,
                         lores=lores, main_yuv=main_yuv, video_yuv=video_yuv,
                         hflip=hflip, vflip=vflip)
        self.cam, self.error, self.seconds = None, None, 0.0
        self.start()

    def run(self):
        t = time.monotonic()
        try:                                   # normal priority: this is the slow part
            os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), 0)
        except (OSError, AttributeError):
            pass
        try:
            self.cam = Camera(**self.args)
        except Exception as e:
            self.error = e
        self.seconds = time.monotonic() - t

    def result(self):
        self.join()
        return self.cam, self.error
