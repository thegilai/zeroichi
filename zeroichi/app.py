"""
Zero Ichi camera app (started by zeroichi.py).

Camera                                 Menu (joystick press)
  KEY1            shutter / record       UP / DOWN      choose setting
  KEY3            PHOTO / GIF / VIDEO    LEFT / RIGHT   change it
  LEFT / RIGHT    change filter          PRESS          run action / close
  UP / DOWN       exposure +/-           KEY2           close
  Joystick press  open the menu        Gallery (KEY2)
  KEY2            open the gallery       LEFT / RIGHT   previous / next
  Hold KEY3 3s    power off              PRESS          play video
                                         KEY3           delete
                                         KEY1 / KEY2    back to camera
"""
import logging
import os
import re
import sys
import threading
from pathlib import Path

# Where Zero Ichi lives on the SD card. (Set by zeroichi.py: these modules may be
# loaded from the compiled cache in /var/cache, so __file__ points there.)
APP_DIR = Path(os.environ.get("ZEROICHI_DIR") or Path(__file__).resolve().parent)
import argparse
import json
import math
import queue
import signal
import subprocess
import time

import numpy as np
from PIL import Image, ImageColor, ImageDraw, ImageFont, ImageOps, ImageSequence

import config
import filters
import camera as camera_sizes  # noqa: E402  (clip size table)
from camera import Camera, CameraLoader, NoCamera
from lcd import ST7789
from movie import MovReader, MovWriter, with_huffman

W = H = 240                    # screen size - updated from the display at start-up
JPEG_QUALITY = 90
VIDEO_QUALITY = 75              # a little lighter per frame than photos
VIDEO_FPS = 15                  # hardware-recorded videos: steady frame rate
EV_MIN, EV_MAX, EV_STEP = -2.0, 2.0, 0.5
HOLD_SECONDS = 3
MODES = ["PHOTO", "GIF", "VIDEO"]
MODE_COLORS = {"PHOTO": (255, 255, 255), "GIF": (120, 220, 255), "VIDEO": (255, 110, 110)}
REC_RED = (255, 60, 60)
REC_RED_565 = int(filters.rgb565(REC_RED))
STATE_FILE = APP_DIR / "state.json"            # everything set in the camera (on the SD card)
PRESETS_DIR = APP_DIR / "presets"               # MENU > Presets: preset-1.txt ... (on the SD card)
NPRESETS = 4
# asks for the fast start (fast-start.sh): power off, normal start-up, make room
EXIT_POWEROFF, EXIT_NORMAL_START, EXIT_MAKE_SPACE = 64, 65, 67
ZEROICHI_STATE = Path("/var/lib/zeroichi")          # fast start files (plan, messages, logs)
PHOTOS_MOUNT = Path("/media/photos")            # the card's big PHOTOS partition
BOOT_PHOTOS = Path("/boot/firmware/photos")     # photos on the small "bootfs" partition
# Picture shape. "Screen" = the same shape as the display (4:3 on a 320x240
# screen, square on 240x240). Photos: 1600x1200 or 1200x1200; GIF/video:
# 320x240 or 240x240. Rotation never changes the shape.
SIZES = ["Screen", "4:3", "Square"]
# (0.1.0) MENU > Image ratio (the setting "size"): what it shows and a preset says
RATIO_LABELS = {"Screen": "Fit screen", "4:3": "4:3", "Square": "1:1"}
RATIO_NAMES = {"screen": "Screen", "fit screen": "Screen", "fit": "Screen", "4:3": "4:3",
               "square": "Square", "1:1": "Square"}
MENU_ROWS = 7                    # rows visible at once; the menu scrolls

# Waveshare 1.3" LCD HAT button pins (BCM), all active-low
PINS = {"up": 6, "down": 19, "left": 5, "right": 26, "press": 13,
        "key1": 21, "key2": 20, "key3": 16}

FONT_PATHS = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
]

log = logging.getLogger("zeroichi")

DEFAULT_STATE = {"mode": "PHOTO", "filter": 0, "ev": 0.0, "cam_rotate": 0, "mirror": False,
                 "screen_flip": False,
                 "gif_frames": 16, "video_max": 30, "size": "Screen",
                 "clip_size": "640x480", "screen": "",
                 "screensaver": "1 min", "space": {}, "screens": {}, "next_number": 1,
                 "space_asked": False, "key3": "Power off"}

SCREENS = ["320x240", "240x240"]          # menu choices; see screen_profiles()
# Screen saver: after this long without a key press the screen and camera go to
# sleep (saves battery, and background saving/developing gets the whole Pi).
SAVER_SECONDS = {"Off": 0, "30 s": 30, "1 min": 60, "2 min": 120, "5 min": 300}
LOW_SPACE = 200 * 1024 * 1024       # free space shown in red below this
MIN_SPACE = 30 * 1024 * 1024        # no new photos / recordings below this
SPACE_AUTO_SECONDS = 10             # Photo space at start-up: the countdown
MAX_VIDEO_BYTES = 3500 << 20        # one video file (the card's FAT drive allows 4 GB at most)
# "How much is left" on the card: bytes per pixel (per frame for GIFs and
# videos) until Zero Ichi has seen your own files - then it uses what they took.
SPACE_DEFAULT = {"photo": 0.20, "gif": 0.50, "video": 0.14}
SPACE_LOOK = dict({"GB Camera": 0.25, "GB Camera B&W": 0.25},
                  **{name: 0.5 for name in filters.PIXEL_LOOKS})


def _onoff(v):
    return "On" if v else "Off"


# (state key, label, choices, how to show the value)
MENU = [                                   # (0.1.0: the screen settings next to Rotation)
    ("presets", "Presets", [""], lambda v: v),
    ("cam_rotate", "Rotation", [0, 90, 180, 270], lambda v: f"{v}\u00b0"),
    ("screen", "Screen", SCREENS, str),
    ("screen_flip", "Flip screen", [False, True], _onoff),
    ("mirror", "Mirror", [False, True], _onoff),
    ("size", "Image ratio", SIZES, lambda v: RATIO_LABELS.get(v, v)),
    ("clip_size", "Clip size", list(camera_sizes.CLIP_SIZES), str),
    ("gif_frames", "GIF frames", [8, 16, 24], str),
    ("video_max", "Video max", [15, 30, 60, 120, 300, 600, 720],          # (12 minutes at most)
     lambda v: f"{v}s" if v < 60 else f"{v // 60} min"),
    ("screensaver", "Screen saver", list(SAVER_SECONDS), str),
    ("photospace", "Photo space", [""], lambda v: photo_space_label()),
    ("erase", "Erase all", [""], lambda v: v),           # (0.1.0: v = "12 files")
    ("key3", "KEY3 hold", ["Power off", "Standby"], str),
    ("poweroff", "Power off", [""], lambda v: "Now"),
    ("about", "About", [""], lambda v: f"v{config.VERSION}"),
]
ACTIONS = {"presets", "about", "photospace", "erase", "poweroff"}   # menu rows that do something on PRESS
from camera import SQUARE_FILTER_NAMES as SQUARE_FILTERS  # noqa: E402  (Holga = 6x6)


def load_font(size):
    for p in FONT_PATHS:
        try:
            return ImageFont.truetype(p, size)
        except OSError:
            continue
    try:
        return ImageFont.load_default(size=size)   # Pillow >= 10.1 built-in font
    except TypeError:
        sys.exit("No TrueType font found. Run: sudo apt install fonts-dejavu-core")


# --- the menus in Zero Ichi's design (v52) ----------------------------------
# A cream card (the live picture still shows around it), the little "0 1" and
# the title, a rule, ink rows; the chosen row an ink bar with a red dot. The
# same as the C program's (app_view.c: ui_*). Its letters: Poppins (fonts/ in
# the zeroichi folder) - or the usual font if it isn't there.
ZI_CREAM, ZI_RED, ZI_YEL = (242, 234, 216), (220, 58, 40), (246, 184, 26)
ZI_BLU, ZI_INK, ZI_GREY = (28, 76, 156), (24, 24, 26), (120, 112, 98)
_brand_fonts = {}


def brand_font(size, bold=False):
    k = (size, bool(bold))
    if k not in _brand_fonts:
        name = "Poppins-Bold.ttf" if bold else "Poppins-Medium.ttf"
        font = None
        for d in (Path(__file__).resolve().parent / "fonts", APP_DIR / "fonts", Path("/boot/firmware/zeroichi/fonts")):
            try:
                font = ImageFont.truetype(str(d / name), size)
                break
            except OSError:
                continue
        _brand_fonts[k] = font or load_font(size)
    return _brand_fonts[k]


_ui_art_cache = {}


def ui_art(kind, w, h):
    """The menus' small shapes, drawn 4x bigger and shrunk (smooth edges), kept:
    an RGBA picture the size of the screen's top (kind "head", "head-up") or of
    a small mark ("dot", "down")."""
    key = (kind, w, h)
    if key in _ui_art_cache:
        return _ui_art_cache[key]
    k = 4
    if kind.startswith("head"):
        im = Image.new("RGBA", (w * k, 38 * k), (0, 0, 0, 0))
        d = ImageDraw.Draw(im)
        c = lambda *v: [x * k for x in v]                         # noqa: E731
        cx, cy, r = 21, 21, 8
        d.ellipse(c(cx - r, cy - r, cx + r, cy + r), fill=ZI_RED)
        d.ellipse(c(cx - r * .42, cy - r * .42, cx + r * .42, cy + r * .42), fill=ZI_CREAM)
        bx = cx + r + 3
        d.rectangle(c(bx, cy - r, bx + r * .5, cy + r), fill=ZI_INK)
        d.polygon([tuple(c(bx, cy - r)), tuple(c(bx, cy - r + r * .5)), tuple(c(bx - r * .44, cy - r + r * .5))],
                  fill=ZI_INK)
        d.pieslice(c(w - 34 - 9, 29 - 9, w - 34 + 9, 29 + 9), 180, 270, fill=ZI_YEL)
        d.rectangle(c(w - 30, 16, w - 22, 24), fill=ZI_BLU)
        if kind == "head-up":
            d.polygon([tuple(c(w - 52, 24)), tuple(c(w - 46, 24)), tuple(c(w - 49, 18))], fill=ZI_RED)
        d.rectangle(c(14, 34, w - 14, 35.5), fill=ZI_INK)
    elif kind == "dot":
        im = Image.new("RGBA", (10 * k, 10 * k), (0, 0, 0, 0))
        ImageDraw.Draw(im).ellipse([(5 - 3.2) * k, (5 - 3.2) * k, (5 + 3.2) * k, (5 + 3.2) * k], fill=ZI_RED)
    else:                                                           # "down": more rows below
        im = Image.new("RGBA", (10 * k, 8 * k), (0, 0, 0, 0))
        ImageDraw.Draw(im).polygon([(1 * k, 1 * k), (9 * k, 1 * k), (5 * k, 7 * k)], fill=ZI_RED)
    art = _ui_art_cache[key] = im.reduce(k)
    return art


def brand_image(lines, title_col=ZI_INK):
    """(v52) A message in Zero Ichi's design: cream, the little "0 1", the title
    (lines up to the first empty one: big) and the text under it - the same as
    the C program's brand_lines."""
    img = Image.new("RGB", (W, H), ZI_CREAM)
    d = ImageDraw.Draw(img)
    nt = next((i for i, ln in enumerate(lines) if not ln), len(lines))
    body = [ln for ln in lines[nt:] if ln]
    r = 9
    y = (H - (2 * r + 12 + nt * 28 + (4 + len(body) * 21 if body else 0))) / 2
    k = 4                                                    # (the mark: drawn 4x, shrunk)
    m = Image.new("RGBA", (int(3 * r + 8) * k, int(2 * r + 4) * k), (0, 0, 0, 0))
    md = ImageDraw.Draw(m)
    cx, cy = (r + 2) * k, (r + 2) * k
    R = r * k
    md.ellipse((cx - R, cy - R, cx + R, cy + R), fill=ZI_RED)
    md.ellipse((cx - .42 * R, cy - .42 * R, cx + .42 * R, cy + .42 * R), fill=ZI_CREAM)
    bx = cx + R + 3 * k
    md.rectangle((bx, cy - R, bx + .5 * R, cy + R), fill=ZI_INK)
    md.polygon([(bx, cy - R), (bx, cy - R + .5 * R), (bx - .44 * R, cy - R + .5 * R)], fill=ZI_INK)
    m = m.reduce(k)
    mx = W / 2 - 0.25 * r - 1.5                              # (the ring's middle)
    img.paste(m, (round(mx - r - 2), round(y - 2)), m)
    y += 2 * r + 12 + 14
    for ln in lines[:nt]:
        f = brand_font(22, True)
        if d.textlength(ln, font=f) > W - 20:
            f = brand_font(16, True)
        d.text((W / 2, y), ln, font=f, fill=title_col, anchor="mm")
        y += 28
    for ln in body:
        f = brand_font(14)
        if d.textlength(ln, font=f) > W - 16:
            f = brand_font(12)
        d.text((W / 2, y), ln, font=f, fill=ZI_BLU if ln.startswith("KEY") else ZI_GREY, anchor="mm")
        y += 21
    return img


def about_image(rows, profile):
    """(v53) MENU > About: a page of its own in the start-up picture's design -
    the mark and the name, this Zero Ichi's details (rows: (label, value) -
    the wide screen's labels, the square one's in brackets) and who made it.
    The same page as the C program's (app_view.c, about_draw)."""
    import brandart
    wide = W > H
    L, R = W * .125, W * .875
    img = Image.new("RGB", (W, H), ZI_CREAM)
    k = 4                                                    # (shapes: drawn 4x, shrunk)
    sh = Image.new("RGBA", (W * k, H * k), (0, 0, 0, 0))
    d = ImageDraw.Draw(sh)
    c = lambda *v: [x * k for x in v]                         # noqa: E731
    s = .40 if wide else .34                                 # (v54: plain cream - no thin lines)
    r, mx = 60 * s, L + 60 * s
    my = 12 + r
    d.ellipse(c(mx - r, my - r, mx + r, my + r), fill=ZI_RED)              # 0
    d.ellipse(c(mx - r * .4, my - r * .4, mx + r * .4, my + r * .4), fill=ZI_CREAM)
    bx = mx + r + 22 * s                                                  # 1
    d.rectangle(c(bx, my - r, bx + 30 * s, my + r), fill=ZI_INK)
    d.polygon([tuple(c(bx, my - r)), tuple(c(bx, my - r + 30 * s)), tuple(c(bx - 26 * s, my - r + 30 * s))],
              fill=ZI_INK)
    q = 42 * s
    d.pieslice(c(bx + 30 * s - q, my + r - q, bx + 30 * s + q, my + r + q), 270, 360, fill=ZI_YEL)
    d.rectangle(c(bx + 46 * s, my + r * .24, bx + 76 * s, my + r * .24 + 30 * s), fill=ZI_BLU)
    for ex in (-14, 14):                                                  # Zero peeking
        x, y, er = mx + ex * s, my - r * .7, 5.2 * s
        d.ellipse(c(x - er, y - er, x + er, y + er), fill=ZI_INK)
        x, y, e2 = x - er * .32, y - er * .35, er * .36
        d.ellipse(c(x - e2, y - e2, x + e2, y + e2), fill=ZI_CREAM)
    y = my + r + 12
    fy = H - 19
    img.paste(sh.reduce(k), (0, 0), sh.reduce(k))
    t = ImageDraw.Draw(img)
    tx = mx - r + 2 * r + 98 * s + (16 if wide else 10)
    title = brand_font(22, True)
    if tx + t.textlength("zero ichi", font=title) > W - 6:
        title = brand_font(16, True)
    t.text((tx, my - 6), "zero ichi", font=title, fill=ZI_INK, anchor="lm")
    kx, ky, kw, kh, kd = brandart.KANA
    kana = Image.frombytes("RGBA", (kw, kh), kd)
    img.paste(kana, (int(tx) + 1, int(my + 8)), kana)
    fl, fv = brand_font(12), brand_font(14 if wide else 12)
    y += 18
    for label, value in rows:
        if not wide and "(" in label:
            label = label[label.index("(") + 1:label.rindex(")")]
        elif "(" in label:
            label = label[:label.index("(")].rstrip()
        t.text((L, y), label, font=fl, fill=ZI_GREY, anchor="lm")
        t.text((R, y), value, font=fv, fill=ZI_INK, anchor="rm")
        y += 21 if wide else 19
    if wide:
        t.text((R, fy), "any key: back", font=brand_font(11), fill=ZI_GREY, anchor="rm")
    return img


def ui_card(d, title, up=False):
    """The card and its header (onto a menu overlay's ImageDraw)."""
    d.rectangle((6, 6, W - 7, H - 7), fill=ZI_CREAM + (255,))
    img = getattr(d, "_image", None)
    if img is not None:
        img.alpha_composite(ui_art("head-up" if up else "head", W, H), (0, 0))
    d.text((43, 21.5), title, font=brand_font(16, True), fill=ZI_INK + (255,), anchor="lm")


def ui_row(d, y, label, value, chosen, label_col=ZI_INK, value_col=ZI_BLU):
    f = brand_font(14)
    if chosen:
        d.rectangle((10, y - 10, W - 11, y + 10), fill=ZI_INK + (255,))
        img = getattr(d, "_image", None)
        if img is not None:
            img.alpha_composite(ui_art("dot", W, H), (12, y - 5))
    d.text((26, y + 0.5), label, font=f, fill=(ZI_CREAM if chosen else label_col) + (255,), anchor="lm")
    if value:
        d.text((W - 18, y + 0.5), value, font=f, fill=(ZI_YEL if chosen else value_col) + (255,), anchor="rm")


def ui_hint(d, text, more=False):
    d.text((14, H - 16), text, font=brand_font(11), fill=ZI_GREY + (255,), anchor="lm")
    img = getattr(d, "_image", None)
    if more and img is not None:
        img.alpha_composite(ui_art("down", W, H), (W - 23, H - 20))


def crop_to_aspect(a, aspect):
    """Largest centred crop of `a` with width/height = aspect (a view, no copy)."""
    h, w = a.shape[:2]
    if abs(w / h - aspect) < 0.01:
        return a
    if w / h > aspect:                                  # too wide: trim the sides
        nw = int(round(h * aspect))
        x = (w - nw) // 2
        return a[:, x:x + nw]
    nh = int(round(w / aspect))                         # too tall: trim top/bottom
    y = (h - nh) // 2
    return a[y:y + nh]


def resize_nearest(a, w, h, cache):
    """Fast nearest-neighbour resize using cached index arrays."""
    key = ("rs", a.shape[0], a.shape[1], w, h)
    idx = cache.get(key)
    if idx is None:
        idx = cache[key] = ((np.arange(h) * a.shape[0] // h)[:, None],
                            (np.arange(w) * a.shape[1] // w)[None, :])
    return a[idx[0], idx[1]]


class Saver(threading.Thread):
    """Processes and saves photos and GIFs in the background, so you can keep
    shooting. At most a couple of jobs wait in line (the Pi Zero has little
    memory); results are reported back to the main loop."""

    def __init__(self, max_waiting=2):
        super().__init__(daemon=True, name="saver")
        self.jobs = queue.Queue(maxsize=max_waiting)
        self.results = queue.Queue()
        self.pending = 0
        self._lock = threading.Lock()
        self._idle = threading.Event()
        self._idle.set()
        self.start()

    def full(self):
        return self.jobs.full()

    def submit(self, fn, *args):
        with self._lock:
            self.pending += 1
            self._idle.clear()
        self.jobs.put((fn, args))                    # waits if the line is full

    def wait(self, timeout=None):
        return self._idle.wait(timeout)

    def run(self):
        set_thread_priority(2)                       # just below the live view
        while True:
            fn, args = self.jobs.get()
            try:
                self.results.put(("ok", fn(*args)))
            except Exception as e:
                log.exception("Background save failed")
                self.results.put(("error", str(e)))
            finally:
                with self._lock:
                    self.pending -= 1
                    if self.pending == 0:
                        self._idle.set()


_HW_BAD = set()          # (w, h, format) the hardware encoder couldn't do
_HW_FMT_FAILS = {}       # format -> sizes it failed for (after 2: not tried again)


class JpegMaker:
    """Makes JPEGs of one size - from R,G,B arrays or from Y/U/V planes (the
    camera's own format: no colour conversion at all). Uses the Pi's hardware
    encoder when it works (each size is tested once), otherwise software."""

    def __init__(self, w, h, quality, bits=2.0):
        self.w, self.h, self.quality, self.bits = w, h, quality, bits
        self.enc = {}
        self.used = set()

    def _hw(self, fmt):
        if fmt not in self.enc and not os.path.exists("/dev/video11") and os.path.exists("/dev/video0"):
            return None          # (v53: its driver loads just after start-up - software this once)
        if fmt not in self.enc:
            key = (self.w, self.h, fmt)
            enc = None
            if key not in _HW_BAD and _HW_FMT_FAILS.get(fmt, 0) < 2:
                try:
                    import hwjpeg
                    enc = hwjpeg.checked(self.w, self.h, bits=self.bits, fmt=fmt)
                except Exception as e:
                    log.info("Hardware JPEG unavailable (%s)", e)
                if enc is None:
                    _HW_BAD.add(key)
                    _HW_FMT_FAILS[fmt] = _HW_FMT_FAILS.get(fmt, 0) + 1
            self.enc[fmt] = enc
        return self.enc[fmt]

    def _failed(self, fmt, e):
        log.warning("Hardware JPEG failed (%s) - compressing in software", e)
        try:
            self.enc[fmt].close()
        except Exception:
            pass
        self.enc[fmt] = None
        _HW_BAD.add((self.w, self.h, fmt))

    def rgb(self, a):
        from movie import encode_jpeg
        a = np.ascontiguousarray(a)
        enc = self._hw("rgb24")
        if enc is not None:
            try:
                data = with_huffman(enc.encode(a))
                self.used.add("hardware")
                return data
            except Exception as e:
                self._failed("rgb24", e)
        self.used.add("software")
        return encode_jpeg(a, self.quality)

    def yuv(self, y, u, v):
        from movie import encode_jpeg_yuv
        enc = self._hw("yuv420")
        if enc is not None:
            try:
                data = with_huffman(enc.encode_yuv(y, u, v))
                self.used.add("hardware")
                return data
            except Exception as e:
                self._failed("yuv420", e)
        self.used.add("software")
        return encode_jpeg_yuv(y, u, v, self.quality)

    def how(self):
        return "+".join(sorted(self.used)) or "-"

    def close(self):
        for enc in self.enc.values():
            if enc is not None:
                try:
                    enc.close()
                except Exception:
                    pass
        self.enc = {}


def _fit_box(w, h, bw, bh):
    """(width, height, x, y) of a w x h picture fitted into bw x bh, centred
    (even numbers, so the colour planes line up)."""
    s = min(bw / w, bh / h)
    fw, fh = min(bw, max(2, int(round(w * s)) & ~1)), min(bh, max(2, int(round(h * s)) & ~1))
    return fw, fh, ((bw - fw) // 2) & ~1, ((bh - fh) // 2) & ~1


class LiveEncoder(threading.Thread):
    """Instant videos: compresses the frames the live view has already made
    (with the look on them) and writes them to the video as they come - so
    there's nothing left to develop afterwards. Runs beside the live view: it
    works while the live view waits for the camera's next frame. If it ever
    falls behind, a frame is skipped rather than holding up the camera."""

    def __init__(self, writer, size, fmt="yuv420"):
        super().__init__(daemon=True, name="video-writer")
        self.writer, self.size, self.fmt = writer, size, fmt
        self.jobs = queue.Queue(maxsize=8)
        self.jm = JpegMaker(size[0], size[1], VIDEO_QUALITY)
        self.frames = self.dropped = 0
        self.spent = 0.0
        self.error = None
        self.start()

    def put(self, item, t):
        try:
            self.jobs.put_nowait((item, t))
        except queue.Full:
            self.dropped += 1

    def run(self):
        try:
            self.jm._hw(self.fmt)                   # get the hardware encoder ready first
        except Exception:
            pass
        while True:
            job = self.jobs.get()
            if job is None:
                return
            if self.error:
                continue
            t0 = time.monotonic()
            try:
                self.writer.add_jpeg(self._encode(job[0]), job[1])
                self.frames += 1
            except Exception as e:
                log.exception("Instant video: a frame could not be written")
                self.error = e
            self.spent += time.monotonic() - t0

    def _encode(self, item):
        kind, obj = item
        bw, bh = self.size
        if kind == "tone":                          # straight to YUV: no colour conversion
            fw, fh, x, y = _fit_box(*obj.size, bw, bh)
            Y, U, V = obj.yuv((fw, fh), smooth=True)
        elif kind == "yuv":                         # (Zero Ichi: its own Y, U, V - enlarged)
            Y, U, V = obj
            fw, fh, x, y = _fit_box(Y.shape[1], Y.shape[0], bw, bh)
            hs = (fw // 2, fh // 2)
            if (Y.shape[1], Y.shape[0]) != (fw, fh):
                Y = np.asarray(Image.fromarray(Y).resize((fw, fh), Image.BILINEAR))
            U, V = (p if (p.shape[1], p.shape[0]) == hs else np.asarray(Image.fromarray(p).resize(hs, Image.BILINEAR))
                    for p in (U, V))
        else:                                       # R,G,B: to YUV while still small
            im = obj if isinstance(obj, Image.Image) else Image.fromarray(np.ascontiguousarray(obj))
            if im.mode != "RGB":
                im = im.convert("RGB")
            fw, fh, x, y = _fit_box(*im.size, bw, bh)
            Y, U, V = im.convert("YCbCr").split()
            Y = np.asarray(Y if Y.size == (fw, fh) else Y.resize((fw, fh), Image.BILINEAR))
            hs = (fw // 2, fh // 2)
            if U.size != hs:                        # (not exactly twice the size: U, V halved
                U, V = U.reduce(2), V.reduce(2)     #  first, then enlarged - like the C program)
            U, V = (np.asarray(p if p.size == hs else p.resize(hs, Image.BILINEAR)) for p in (U, V))
        if (fw, fh) != (bw, bh):                    # other shape (e.g. square look): bars
            Yc = np.zeros((bh, bw), np.uint8)
            Uc = np.full((bh // 2, bw // 2), 128, np.uint8)
            Vc = Uc.copy()
            Yc[y:y + fh, x:x + fw] = Y
            Uc[y // 2:y // 2 + fh // 2, x // 2:x // 2 + fw // 2] = U
            Vc[y // 2:y // 2 + fh // 2, x // 2:x // 2 + fw // 2] = V
            Y, U, V = Yc, Uc, Vc
        return self.jm.yuv(np.ascontiguousarray(Y), np.ascontiguousarray(U),
                           np.ascontiguousarray(V))

    def finish(self, timeout=10.0):
        """Write the frames still waiting, then stop."""
        self.jobs.put(None)
        self.join(timeout)
        self.jm.close()


class FrameDeveloper:
    """Develops the frames of one recording: decode, filter, compress. Black &
    white looks work on the brightness plane only and go straight to YUV."""

    def __init__(self, rd, name, w, h, turn):
        self.rd, self.name, self.w, self.h, self.turn = rd, name, w, h, turn
        self.fn = dict(filters.FILTERS).get(name, filters.normal)
        self.tone = filters.TONE_FILTERS.get(name)
        self.ctone = filters.COLOR_TONES.get(name)
        sw, sh = rd.size
        self.half = (sw // 2, sh // 2) if name in filters.HALF_SIZE_OK else None
        self.jm = JpegMaker(w, h, VIDEO_QUALITY)
        self.spent = [0.0, 0.0, 0.0]                # decode, filter, compress

    def frame(self, i, want_preview=False):
        """(JPEG bytes, small RGB preview or None) for frame i."""
        w, h = self.w, self.h
        t0 = time.monotonic()
        if self.ctone is not None:                  # 16-Bit: decoded small, as YUV
            sw, sh = self.rd.size
            ycc = self.rd.frame_ycc(i, filters.pixel_grid(sw, sh))
            if self.turn:
                ycc = ycc.transpose(Image.Transpose.ROTATE_270)
            t1 = time.monotonic()
            tn = self.ctone(ycc, (w, h))
            planes = tn.yuv()
            preview = np.asarray(tn.rgb()) if want_preview else None
            t2 = time.monotonic()
            data = self.jm.yuv(*planes)
            self.spent[0] += t1 - t0
            self.spent[1] += t2 - t1
            self.spent[2] += time.monotonic() - t2
            return data, preview
        if self.tone is not None:                   # brightness only: no colour work
            a = self.rd.frame_gray(i, self.half)
        else:
            a = self.rd.frame_array(i, self.half)   # half size: decoded almost for free
        if self.turn:
            a = np.rot90(a, -1)
        t1 = time.monotonic()
        preview = None
        if self.tone is not None:
            tn = self.tone(Image.fromarray(np.ascontiguousarray(a)), (w, h))
            planes = tn.yuv()                       # straight to YUV: no RGB anywhere
            if want_preview:
                preview = np.asarray(tn.rgb())
            t2 = time.monotonic()
            data = self.jm.yuv(*planes)
        else:
            out = filters.for_clip(self.name, self.fn, np.ascontiguousarray(a))
            if filters.size_of(out) != (w, h):      # enlarge once, back to full size
                out = filters._pil(out).resize((w, h), Image.NEAREST)
            out = np.ascontiguousarray(filters.as_array(out))
            preview = out if want_preview else None
            t2 = time.monotonic()
            data = self.jm.rgb(out)
        self.spent[0] += t1 - t0
        self.spent[1] += t2 - t1
        self.spent[2] += time.monotonic() - t2
        return data, preview

    def close(self):
        self.jm.close()


def develop_video(raw, set_progress, hold=None):
    """Apply the filter to a hardware-recorded video, frame by frame ("develop
    the film"). The raw recording and its note stay until this has finished, so
    it can be picked up again after a power-off."""
    note_path = raw.with_name(raw.name + ".json")
    note = json.loads(note_path.read_text())
    final = raw.parent / note["final"]
    name = note["filter"]
    if name not in dict(filters.FILTERS):          # (recorded by an older version)
        pal = note.get("palette")
        name = f"16-Bit {pal}" if f"16-Bit {pal}" in filters.PIXEL_LOOKS else \
            filters.OLD_NAMES.get(name, "Normal")
    t_start = time.monotonic()
    rd = MovReader(str(raw))
    n = len(rd)
    w, h = rd.size
    turn = rd.rotation == 90
    if turn:
        w, h = h, w
    tmp = final.with_name(final.name + ".dev")
    wr = MovWriter(str(tmp), w, h, quality=VIDEO_QUALITY, look=name)
    fd = FrameDeveloper(rd, name, w, h, turn)
    t = 0.0
    for i in range(n):
        while hold is not None and hold():          # recording: wait (it gets the CPU)
            time.sleep(0.2)
        data, preview = fd.frame(i, want_preview=i % 5 == 0)
        wr.add_jpeg(data, t)
        t += rd.durations[i]
        set_progress((i + 1) / n, preview)
    wr.close()
    rd.close()
    fd.close()
    os.replace(tmp, final)
    raw.unlink(missing_ok=True)
    note_path.unlink(missing_ok=True)
    os.sync()
    log.info("Developed %s (%s, %d frames, %dx%d) in %.0f s; per frame: decode %.0f ms, "
             "filter %.0f ms, compress %.0f ms (%s)", final.name, name, n, w, h,
             time.monotonic() - t_start, *(1000 * x / max(n, 1) for x in fd.spent), fd.jm.how())
    try:
        nbytes = final.stat().st_size
    except OSError:
        nbytes = 0
    return ("Video saved", final, ("video", name, w * h * n, nbytes))


class Developer(threading.Thread):
    """Develops filtered videos in the background, one at a time."""

    def __init__(self):
        super().__init__(daemon=True, name="developer")
        self.jobs = queue.Queue()
        self.results = queue.Queue()
        self.pending = 0
        self.progress = None
        self.preview = None
        self.hold = None                         # set by the app: True while recording
        self.start()

    def submit(self, raw):
        self.pending += 1
        self.jobs.put(raw)

    def _set(self, p, frame=None):
        self.progress = p
        if frame is not None:
            self.preview = frame[::2, ::2].copy()      # for the "developing" screen

    def run(self):
        set_thread_priority(10)                      # yield to the live view
        while True:
            raw = self.jobs.get()
            self.progress = 0.0
            try:
                self.results.put(("ok", develop_video(raw, self._set, self.hold)))
            except Exception as e:
                log.exception("Developing %s failed", raw.name)
                self.results.put(self._failed(raw, e))
            finally:
                self.pending -= 1
                self.progress = None

    @staticmethod
    def _failed(raw, err):
        """Never lose a video: after a second failed try, keep it unfiltered (the
        raw recording is a complete video already, rotation flag included)."""
        note_path = raw.with_name(raw.name + ".json")
        try:
            note = json.loads(note_path.read_text())
            note["failures"] = note.get("failures", 0) + 1
            if note["failures"] < 2:
                note_path.write_text(json.dumps(note))     # try again next start
                return ("error", str(err))
            final = raw.parent / note["final"]
            os.replace(raw, final)
            note_path.unlink(missing_ok=True)
            final.with_name(final.name + ".dev").unlink(missing_ok=True)
            os.sync()
            log.warning("Kept %s without the filter (developing failed twice)", final.name)
            return ("ok", ("Video saved (no filter)", final))
        except Exception:
            log.exception("Could not keep %s", raw.name)
            return ("error", str(err))


_ROLES = {"MainThread": "live view", "lcd-writer": "screen sender", "saver": "saving",
          "video-writer": "video writer",
          "developer": "developing", "buttons": "buttons"}


def _thread_role(name, comm):
    """A readable name for a thread in the CPU report."""
    if name is None:                                     # not a Python thread: libcamera
        return "libcamera" if comm in ("python3", "python") else f"libcamera ({comm})"
    if name in _ROLES:
        return _ROLES[name]
    if "thread_poll" in name:
        return "video encoder"
    if "(thread_func)" in name or "(listen)" in name:
        return "camera (picamera2)"
    return name


class CpuMeter:
    """Where the processor's time went, per thread (read from Linux's /proc) -
    written to the log, so slow spots can be found on the real Pi Zero."""

    def __init__(self):
        self.start = self._read()

    @staticmethod
    def _read():
        try:
            tick = float(os.sysconf("SC_CLK_TCK"))
        except (ValueError, OSError, AttributeError):
            tick = 100.0
        threads = {}
        try:
            tids = os.listdir("/proc/self/task")
        except OSError:
            tids = []
        for tid in tids:
            try:
                with open(f"/proc/self/task/{tid}/stat", "rb") as f:
                    st = f.read().decode("latin-1")
                r = st.rfind(")")
                fields = st[r + 2:].split()
                threads[int(tid)] = (st[st.find("(") + 1:r],
                                     (int(fields[11]) + int(fields[12])) / tick)
            except (OSError, ValueError, IndexError):
                continue
        busy = idle = 0.0
        try:
            with open("/proc/stat") as f:
                v = [int(x) for x in f.readline().split()[1:9]]
            idle = (v[3] + v[4]) / tick
            busy = sum(v) / tick - idle
        except (OSError, ValueError, IndexError):
            pass
        return threads, busy, idle, time.monotonic()

    def report(self, restart=True):
        """e.g. "live view 41%, camera (picamera2) 12%, ..., idle 3%" since the
        start (or the last report)."""
        now = self._read()
        (th0, busy0, idle0, t0), (th1, busy1, idle1, t1) = self.start, now
        if restart:
            self.start = now
        wall = max(1e-3, t1 - t0) * max(1, os.cpu_count() or 1)   # % of the whole processor
        names = {}
        for t in threading.enumerate():
            if getattr(t, "native_id", None) is not None:
                names[t.native_id] = t.name
        groups, ours = {}, 0.0
        for tid, (comm, cpu) in th1.items():
            d = cpu - th0.get(tid, (comm, 0.0))[1]
            if d <= 0:
                continue
            ours += d
            role = _thread_role(names.get(tid), comm)
            groups[role] = groups.get(role, 0.0) + d
        pct = lambda x: 100.0 * x / wall                            # noqa: E731
        parts = [f"{k} {pct(v):.0f}%" for k, v in sorted(groups.items(), key=lambda kv: -kv[1])
                 if pct(v) >= 0.5]
        parts.append(f"rest of the system {pct(max(0.0, busy1 - busy0 - ours)):.0f}%")
        parts.append(f"idle {pct(max(0.0, idle1 - idle0)):.0f}%")
        return ", ".join(parts)


def _fstab_has(mount_point):
    try:
        return any(ln.split()[1:2] == [str(mount_point)] for ln in
                   Path("/etc/fstab").read_text().splitlines() if not ln.lstrip().startswith("#"))
    except OSError:
        return False


def _sys_int(path):
    with open(_SYS + path) as f:
        return int(f.read().strip())


_SYS = os.environ.get("ZEROICHI_TEST_SYS", "")       # (tests: a made-up /sys and /proc)
MIB4 = 8192                                          # 4 MiB in sectors
GIB_S = 2097152                                      # 1 GiB in sectors


def _up4(s):
    return -(-s // MIB4) * MIB4


def whole_card():
    """The whole card holds photos already: a PHOTOS drive, or a big start-up
    drive (ZEROICHI, which took over from bootfs)."""
    if os.path.ismount(PHOTOS_MOUNT):
        return True
    try:
        st = os.statvfs(BOOT_PHOTOS.parent)
        return st.f_blocks * st.f_frsize >= 2 << 30
    except OSError:
        return False


def space_fails():
    try:
        return int((ZEROICHI_STATE / "space-fails").read_text().strip() or 0)
    except (OSError, ValueError):
        return 0


def space_plan():
    """How the card gets room for photos (carried out by the fast start:
    fast-start.sh, make_space). Returns (plan, None) or (None, reason).
      move   - the system partition is made smaller and moved to the end of
               the card; the space becomes ONE big FAT drive (ZEROICHI) that takes
               over from bootfs with all its files
      fat    - the same when the system is at the end already (a first try
               stopped half-way)
      photos - cards too small to copy the system: an extra PHOTOS drive"""
    import shutil
    if not _SYS and (whole_card() or _fstab_has(PHOTOS_MOUNT)):
        return None, "done"
    if os.environ.get("ZEROICHI_FASTINIT") != "1":
        return None, "needs fast_start = yes"
    if os.geteuid() != 0 and not _SYS:
        return None, "needs to run as root"
    try:
        m = re.search(r"\broot=PARTUUID=([0-9a-f]{8})-02\b", Path(_SYS + "/proc/cmdline").read_text())
        if not m:
            return None, "unusual card layout"
        diskid = m.group(1)
        if _SYS:
            part = Path(_SYS + "/rootpart").read_text().strip()
        else:
            st = os.stat("/")
            part = os.path.basename(os.path.realpath(f"/sys/dev/block/{os.major(st.st_dev)}:"
                                                     f"{os.minor(st.st_dev)}"))
        if not re.fullmatch(r"mmcblk\dp2", part):
            return None, "unusual card layout"
        disk = part[:-2]
        parts = {}
        for i in range(1, 9):
            try:
                parts[i] = (_sys_int(f"/sys/block/{disk}/{disk}p{i}/start"),
                            _sys_int(f"/sys/block/{disk}/{disk}p{i}/size"))
            except (OSError, ValueError):
                pass
        disk_size = _sys_int(f"/sys/block/{disk}/size")
        if _SYS:
            used = int(Path(_SYS + "/used").read_text())
        else:
            vfs = os.statvfs("/")
            used = (vfs.f_blocks - vfs.f_bfree) * vfs.f_frsize
    except (OSError, ValueError) as e:
        return None, f"card not readable ({e})"
    if 1 not in parts or 2 not in parts:
        return None, "card not readable"
    if any(i in parts for i in range(4, 9)):
        return None, "the card already has other partitions"
    (s1, n1), (s2, n2) = parts[1], parts[2]
    p1_end, gap = s1 + n1, s2 - (s1 + n1)
    at_end = s2 + n2 + 2 * MIB4 >= disk_size
    if 3 in parts and not (parts[3][0] >= p1_end and sum(parts[3]) <= s2):
        return None, "the card already has other partitions"     # (one in the gap: a first try)
    # the system partition's new size: what it holds, a quarter more and 1 GiB
    # to spare - at least 4 GiB (whole 4 MiB)
    target = max(4 << 30, int(used * 1.25) + (1 << 30))
    target = -(-target // (4 << 20)) * (4 << 20)
    plan = {"DISK": f"/dev/{disk}", "PART": f"/dev/{part}", "DISKID": diskid, "P1_START": s1,
            "P1_SIZE": n1, "ROOT_START": s2, "ROOT_SIZE": n2}
    if gap >= 2 * GIB_S and at_end and n2 % 2 == 0:
        fat_start = _up4(p1_end)                     # the system is at the end already
        plan.update(MODE="fat", ROOT_KB=n2 // 2, ROOT_SECTORS=n2, ROOT_NEW_START=s2,
                    FAT_START=fat_start, FAT_SECTORS=s2 - fat_start, system=n2 * 512)
    elif gap < 64 * 2048 and 3 not in parts:         # as Raspberry Pi OS left it
        if target // 512 > n2:                       # (not grown: a flashed card image)
            target = n2 // MIB4 * MIB4 * 512
        root_sectors = target // 512
        new_start = (disk_size - root_sectors) // MIB4 * MIB4
        fat_start = _up4(s2)
        plan.update(ROOT_KB=target // 1024, ROOT_SECTORS=root_sectors, system=target)
        if root_sectors <= n2 and new_start >= s2 + root_sectors and new_start - fat_start >= 2 * GIB_S:
            plan.update(MODE="move", ROOT_NEW_START=new_start, FAT_START=fat_start,
                        FAT_SECTORS=new_start - fat_start)
        else:                                        # too small to copy the system
            p3 = _up4(s2 + root_sectors)
            if root_sectors > n2 or (disk_size - p3) < 3 * GIB_S // 2:
                return None, "the card is too small"
            plan.update(MODE="photos", P3_START=p3)
    else:
        return None, "unusual card layout"
    plan["gain"] = ((disk_size - plan["P3_START"]) if plan["MODE"] == "photos"
                    else plan["FAT_SECTORS"]) * 512
    if plan["MODE"] == "move":                       # (copying the system: ~9 MB/s on a Pi Zero)
        plan["minutes"] = int(math.ceil((plan["system"] / 9e6 / 60 + 4) / 5) * 5)
    else:
        plan["minutes"] = 2 if plan["MODE"] == "fat" else 10
    if plan["MODE"] != "photos" and int(os.environ.get("ZEROICHI_INIT") or 0) < 3:
        return None, "please restart Zero Ichi first"   # (an older fast start is running)
    if not _SYS:
        missing = [t for t in ("e2fsck", "resize2fs", "sfdisk", "partx", "pivot_root", "chroot", "ldd",
                               "mkfs.fat", "blkid", "findfs", "dd", "cmp", "cp", "find")
                   if not shutil.which(t)]
        if missing:
            return None, "missing tools: " + ", ".join(missing)
    return plan, None


PLAN_KEYS = ("MODE", "DISK", "PART", "DISKID", "P1_START", "P1_SIZE", "ROOT_START", "ROOT_SIZE",
             "ROOT_KB", "ROOT_SECTORS", "ROOT_NEW_START", "FAT_START", "FAT_SECTORS", "P3_START")


def photo_space_label():
    whole = whole_card()
    if W < 300:                                      # (short words on the small screen)
        return "All" if whole else "More"
    return "Whole card" if whole else "Get more"


def gb(n):
    return f"{n / 1e9:.1f} GB"


def clock_from_card():
    """The Pi has no clock battery: carry on from the last time Zero Ichi wrote
    its own files on the card (as the C Zero Ichi does)."""
    try:
        files = (APP_DIR / "zeroichi-log.txt", STATE_FILE, APP_DIR / "boot-time.txt")
        best = max((p.stat().st_mtime for p in files if p.exists()), default=0)
        if best > time.time() + 1 and os.geteuid() == 0:
            time.clock_settime(time.CLOCK_REALTIME, best + 1)
    except (OSError, AttributeError):
        pass


def photo_space_needed():
    """MENU > Photo space is only shown while it can still do something."""
    return not whole_card() and not _fstab_has(PHOTOS_MOUNT)


# -- presets: the picture settings as small "key = value" files on the card --
def preset_path(i):
    return PRESETS_DIR / f"preset-{i + 1}.txt"


def preset_write(st, path, number, screen, camera=""):
    """Save every setting (the look, mode, EV and every MENU setting) - easy to
    read, edit and copy. screen: "320x240" / "240x240"; camera: the Pi serial
    of the Zero Ichi it is saved on (its screen is only used there)."""
    onoff = {True: "on", False: "off"}
    text = (f"# Zero Ichi preset {number} (saved {time.strftime('%Y-%m-%d %H:%M')})\n"
            f"# Load it on the camera: MENU > Presets, choose {number}, KEY1.\n"
            "# You can edit this file on a computer, or copy it to another Zero Ichi.\n"
            f"look = {filters.FILTERS[st['filter']][0]}\nmode = {st['mode']}\nev = {st['ev']:+.1f}\n"
            f"rotation = {st['cam_rotate']}\nimage_ratio = {RATIO_LABELS.get(st['size'], st['size'])}\n"
            f"mirror = {onoff[bool(st['mirror'])]}\n"
            f"flip_screen = {onoff[bool(st['screen_flip'])]}\n"
            f"clip_size = {st['clip_size']}\ngif_frames = {st['gif_frames']}\nvideo_max = {st['video_max']}\n"
            f"screen = {screen}\nscreen_saver = {st['screensaver']}\nkey3_hold = {st['key3']}\n"
            "# (the Zero Ichi it was saved on: its screen is only used there - another\n"
            "#  Zero Ichi keeps its own screen)\n"
            f"camera = {camera or '-'}\n")
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
    os.sync()


def preset_read(st, path):
    """A preset's values on top of state `st` (a copy is returned; "_screen":
    the screen it was saved with, "_camera": the Zero Ichi it was saved on - if
    the file says), or None."""
    try:
        text = path.read_text(errors="replace")
    except OSError:
        return None
    names = [n for n, _ in filters.FILTERS]
    lower = {n.lower(): n for n in names}
    out, got = dict(st), 0
    choices = {k: c for k, _, c, _ in MENU}
    for line in text.splitlines():
        line = line.split("#", 1)[0]
        if "=" not in line:
            continue
        k, v = (x.strip() for x in line.split("=", 1))
        k = k.lower().replace("-", "_").replace(" ", "_")
        if not v:
            continue
        got += 1
        try:
            if k in ("look", "filter"):
                n = {o.lower(): x for o, x in filters.OLD_NAMES.items()}.get(v.lower(), v)
                n = n if n in names else lower.get(n.lower())
                if n:
                    out["filter"] = names.index(n)
            elif k == "mode" and v.upper() in MODES:
                out["mode"] = v.upper()
            elif k == "ev":
                out["ev"] = min(EV_MAX, max(EV_MIN, round(float(v) * 2) / 2))
            elif k in ("size", "image_ratio", "ratio"):
                out["size"] = RATIO_NAMES.get(v.lower(), out["size"])
            elif k in ("mirror", "flip_screen", "screen_flip"):
                out["mirror" if k == "mirror" else "screen_flip"] = v.lower() in ("1", "on", "yes", "true")
            elif k in ("rotation", "cam_rotate") and int(v) in choices["cam_rotate"]:
                out["cam_rotate"] = int(v)
            elif k in ("screen_saver", "screensaver"):
                out["screensaver"] = next((x for x in SAVER_SECONDS if x.lower() == v.lower()),
                                          out["screensaver"])
            elif k in ("key3_hold", "key3"):
                out["key3"] = next((x for x in ("Power off", "Standby") if x.lower() == v.lower()), out["key3"])
            elif k == "screen" and v in SCREENS:
                out["_screen"] = v
            elif k == "camera":
                out["_camera"] = v
                got -= 1                                    # (not a setting)
            elif k == "clip_size" and camera_sizes.OLD_CLIP_NAMES.get(v, v) in choices["clip_size"]:
                out["clip_size"] = camera_sizes.OLD_CLIP_NAMES.get(v, v)
            elif k in ("gif_frames", "video_max") and int(v.rstrip("s")) in choices[k]:
                out[k] = int(v.rstrip("s"))
            else:
                got -= 1
        except ValueError:
            got -= 1
    return out if got else None


def preset_summary(st):
    return f"{filters.FILTERS[st['filter']][0]} \u2022 {st['mode']}"


def fast_start_running():
    """Started by Zero Ichi's own fast start-up (fast-start.sh), not systemd."""
    try:
        import bootopt
        return bootopt.fast_start_running()
    except Exception:
        return os.environ.get("ZEROICHI_FASTINIT") == "1"


_THROTTLE_BITS = {0: "under-voltage", 1: "speed capped", 2: "throttled", 3: "temperature limit"}


def _read_first(paths):
    import glob
    for pat in paths:
        for p in sorted(glob.glob(pat)):
            try:
                with open(p) as f:
                    return f.read().strip()
            except OSError:
                continue
    return None


def pi_health():
    """Temperature, processor speed and any power / heat problems the Pi
    reports (a weak power supply or a hot Pi slows everything down), e.g.
    "48 C, 1000 MHz" or "71 C, 700 MHz, under-voltage now"."""
    parts = []
    t = _read_first(["/sys/class/thermal/thermal_zone0/temp"])
    if t and t.lstrip("-").isdigit():
        parts.append(f"{int(t) / 1000:.0f} C")
    f = _read_first(["/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"])
    if f and f.isdigit():
        parts.append(f"{int(f) // 1000} MHz")
    th = _read_first(["/sys/devices/platform/soc/soc:firmware/get_throttled",
                      "/sys/devices/platform/soc*/*firmware*/get_throttled"])
    try:
        bits = int(th, 16) if th else None
    except ValueError:
        bits = None
    if bits:
        now = [n for b, n in _THROTTLE_BITS.items() if bits >> b & 1]
        before = [n for b, n in _THROTTLE_BITS.items() if bits >> (b + 16) & 1 and not bits >> b & 1]
        if now:
            parts.append(", ".join(now) + " now")
        if before:
            parts.append(", ".join(before) + " earlier")
    elif bits is None:
        alarm = _read_first(["/sys/class/hwmon/hwmon*/in0_lcrit_alarm"])
        if alarm == "1":
            parts.append("under-voltage now")
    return ", ".join(parts) or "-"


def set_cpu_speed(governor):
    """"performance" = full speed; "powersave" = slowest (standby)."""
    import glob
    for g in glob.glob("/sys/devices/system/cpu/cpu*/cpufreq/scaling_governor"):
        try:
            with open(g, "w") as f:
                f.write(governor)
        except OSError:
            pass


def set_thread_priority(nice):
    """Linux priorities are per thread: background work yields to the live view."""
    try:
        os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), nice)
    except (OSError, AttributeError):
        pass


def make_photo(full, snap):
    """Camera picture -> finished photo (turned, cropped, scaled and filtered),
    using a snapshot of the settings from when it was taken. Returns
    ("yuv", Y, U, V) - saved with no colour conversion at all - or
    ("img", picture)."""
    if hasattr(full, "planes") and not full.swap_rb:     # the photo stream (YUV)
        sw = not snap["hw_flip"]
        Y, U, V = full.planes(snap["size"], snap["turn"], sw and snap["hflip"], sw and snap["vflip"])
        w, h = snap["size"]
        if Y.shape != (h, w):                            # (only if the stream is smaller)
            Y, U, V = (np.asarray(Image.fromarray(p).resize(s, Image.BILINEAR))
                       for p, s in ((Y, (w, h)), (U, (w // 2, h // 2)), (V, (w // 2, h // 2))))
        return filters.photo_from_yuv(snap["filter_name"], Y, U, V)
    a = full.rgb() if hasattr(full, "rgb") else full
    if not snap["hw_flip"]:
        if snap["hflip"]:
            a = a[:, ::-1]
        if snap["vflip"]:
            a = a[::-1]
    aspect = snap["aspect"]
    a = crop_to_aspect(a, 1 / aspect if snap["turn"] else aspect)
    img = Image.fromarray(np.ascontiguousarray(a))
    if snap["turn"]:
        img = img.transpose(Image.Transpose.ROTATE_270)      # 90 degrees clockwise
    if img.size != snap["size"]:
        img = img.resize(snap["size"], Image.BILINEAR, reducing_gap=2.0)
    return ("img", snap["filter"](img))


class SaverArt:
    """The screen saver's picture (v51, the Bauhaus "W" design): shapes drifting
    and turning slowly behind a sleeping "0 1" - the same design as the C
    program's screensaver.c (drawn twice as big and halved: smooth edges)."""
    SPEED = 3.0                     # the design's time units a second
    CREAM, RED, YEL, BLU, BLK = (242, 234, 216), (220, 58, 40), (246, 184, 26), (28, 76, 156), (24, 24, 26)
    # kind, colour, size; where it starts and which way it drifts (the design's random.Random(5))
    SHAPES = [("circle", RED, 24, 0.6229016948897019, 0.7417869892607294, 0.5903871311313933, 0.8849005675541006),
              ("quarter", YEL, 38, 0.7398985747399307, 0.922324996665417, -0.9419895434327705, -0.0687546912437893),
              ("square", BLU, 20, 0.9433567169983137, 0.6489745531369242, 0.8018009835012454, -0.7735880706937113),
              ("bar", CREAM, 40, 0.46906904778216374, 0.24657283261983032, 0.08752171847186085, 0.14788237585620156),
              ("tri", RED, 26, 0.013114189588902203, 0.21672980046384815, -0.44103526797777937, 0.8326907436171038),
              ("circle", YEL, 16, 0.7657254516291417, 0.15960421235803823, 0.594293982862409, -0.7224651632021937),
              ("ring", BLU, 20, 0.6174525204661166, 0.1266992325502697, -0.9964502755949307, 0.7428094894485642),
              ("square", CREAM, 10, 0.2094563824951179, 0.21548116922473226, 0.9648422176518505, 0.7448155308736037)]
    CORNERS = {"square": [(-1, -1), (1, -1), (1, 1), (-1, 1)], "bar": [(-.22, -1), (.22, -1), (.22, 1), (-.22, 1)],
               "tri": [(-1, .8), (1, .8), (0, -.95)]}

    def __init__(self, w, h):
        import brandart                     # (the sleeping "0 1" and the text: pictures)
        self.w, self.h = w, h
        self.art = []
        for (dx, dy, aw, ah, data), y0 in ((brandart.MARK, h // 2), (brandart.TEXT, h)):
            self.art.append((Image.frombytes("RGBA", (aw, ah), data), (w // 2 + dx, y0 + dy)))

    def frame(self, seconds):
        w, h, k = self.w, self.h, 2
        t = seconds * self.SPEED
        im = Image.new("RGB", (w * k, h * k), self.BLK)
        d = ImageDraw.Draw(im)
        for i, (kind, col, size, px, py, vx, vy) in enumerate(self.SHAPES):
            x = ((px + vx * 0.018 * t) % 1.4 - .2) * w          # (wrapping around past the edges)
            y = ((py + vy * 0.018 * t) % 1.4 - .2) * h
            a = (t * 6 * (1 if i % 2 else -1) + i * 40) % 360
            box = [k * (x - size), k * (y - size), k * (x + size), k * (y + size)]
            if kind in ("circle", "ring"):
                d.ellipse(box, fill=col)
                if kind == "ring":
                    r = size * .45
                    d.ellipse([k * (x - r), k * (y - r), k * (x + r), k * (y + r)], fill=self.BLK)
            elif kind == "quarter":
                d.pieslice(box, a, a + 90, fill=col)
            else:
                ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
                d.polygon([(k * (x + size * (cx * ca - cy * sa)), k * (y + size * (cx * sa + cy * ca)))
                           for cx, cy in self.CORNERS[kind]], fill=col)
        im = im.reduce(k)
        for pic, pos in self.art:
            im.paste(pic, pos, pic)
        return im


SAVER_FPS = 6                       # the screen saver: pictures a second (Python)
SAVER_DARK_AFTER = 600              # ... and the screen off after 10 minutes of it


class Overlay:
    """Pixels drawn over the picture (text, menu...): where they go and their
    colours - kept ready for both screen formats (R,G,B bytes: setting single
    bytes is several times faster than setting whole pixels; 16-bit: one
    number per pixel). Half-transparent pixels (`dim`) darken the picture."""
    __slots__ = ("px", "rgb", "dim", "_b3", "_w")

    def __init__(self, pixels, colours, dim=None):
        self.px = np.asarray(pixels, np.intp)
        self.rgb = np.ascontiguousarray(colours, np.uint8).reshape(-1, 3)
        self.dim = None if dim is None else np.asarray(dim, np.intp)
        self._b3 = self._w = None

    def bytes3(self):
        if self._b3 is None:
            def bytes_of(px):
                return (px[:, None] * 3 + np.arange(3)).reshape(-1)
            self._b3 = (self.rgb.reshape(-1), bytes_of(self.px),
                        None if self.dim is None else bytes_of(self.dim))
        return self._b3

    def words(self):
        if self._w is None:
            self._w = (filters.rgb565(self.rgb), self.px, self.dim)
        return self._w


_PAIR = np.uint32(0x10001)       # v * _PAIR = v twice, side by side (16-bit pixels)


def dim565(raw):
    """16-bit screen pixels (as stored for the screen) at about a third of the
    brightness - for the menu's see-through background."""
    v = raw.byteswap()
    v = ((v >> 2) & 0x39E7) + ((v >> 4) & 0x0861)    # 1/4 + 1/16 of each channel
    return v.byteswap()


def rgb_to_565(a):
    """R,G,B array (h x w x 3) -> 16-bit screen pixels (h x w), with 8-bit
    operations (quick on a Pi Zero). Live view: done on the small picture,
    before it is enlarged for the screen."""
    h, w = a.shape[:2]
    buf = np.empty((h, w, 2), np.uint8)
    tmp = np.empty((h, w), np.uint8)
    r, g, b = a[..., 0], a[..., 1], a[..., 2]
    hi, lo = buf[..., 0], buf[..., 1]
    np.bitwise_and(r, 0xF8, out=hi)                  # RRRRRGGG
    np.right_shift(g, 5, out=tmp)
    np.bitwise_or(hi, tmp, out=hi)
    np.left_shift(g, 3, out=tmp)                     # GGGBBBBB
    np.bitwise_and(tmp, 0xE0, out=tmp)
    np.right_shift(b, 3, out=lo)
    np.bitwise_or(lo, tmp, out=lo)
    return buf.view(np.uint16)[..., 0]


_MEDIA_NUM = re.compile(r"^(?:IMG|GIF|VID)_(\d{1,7})\.")


def _media_number(name):
    """The number in IMG_0042.jpg / VID_0042.part (None for other names)."""
    m = _MEDIA_NUM.match(name)
    return int(m.group(1)) if m else None


def _media_order(p):
    """Shooting order: older versions' dated names (IMG_20261002_041056.jpg)
    first, then the numbered ones."""
    n = _media_number(p.name)
    return (0, p.name[4:], p.name) if n is None else (1, n, p.name)


def center_square(a):
    h, w = a.shape[:2]
    s = min(h, w)
    y, x = (h - s) // 2, (w - s) // 2
    return a[y:y + s, x:x + s]


# --------------------------------------------------------------------------
# --------------------------------------------------------------------------
class Controls:
    """Turns button presses into (name, kind) events on a queue."""

    FLIP = {"up": "down", "down": "up", "left": "right", "right": "left"}

    def __init__(self, events, flip=False, pins=None):
        import gpio
        self.events = events
        self.flip = flip            # swap joystick directions when the screen is flipped
        self.poller = gpio.ButtonPoller(pins or PINS, self._event, hold_names=("key3",),
                                        hold_time=HOLD_SECONDS)

    def _map(self, name):
        return self.FLIP.get(name, name) if self.flip else name

    def _event(self, name, kind):
        self.events.put((self._map(name), kind))

    def close(self):
        self.poller.close()


# --------------------------------------------------------------------------
class ZeroIchi:
    def __init__(self, args, lcd=None, camera_loader=None, progress=None):
        self.args = args
        self.rec = None                         # active GIF / video recording
        self.hw_video_failed = False
        self.video_rgb = False                  # True: record from an RGB stream (fallback)
        self._pace_start = 0.08                 # live view while recording: see rec_pace()
        self.ready_after = None
        self.photo_dir = self.photo_dir_setting = None
        self._photo_setting = str(args.photo_dir or "auto")
        self._space_setting = str(getattr(args, "photo_space", "auto") or "auto").lower()
        if self._photo_setting not in ("auto", str(BOOT_PHOTOS)):
            self.photo_dir = Path(self._photo_setting).expanduser()
        else:                                    # (decided once the screen is on)
            self.photo_dir = PHOTOS_MOUNT if os.path.ismount(PHOTOS_MOUNT) else BOOT_PHOTOS
        try:
            self.photo_dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass

        self.f14, self.f16, self.f18, self.f22, self.f30 = (
            load_font(s) for s in (14, 16, 18, 22, 30))
        clock_from_card()
        self.state = self.load_state()
        self._geo_cache = {}                    # see _geo()

        global W, H
        if lcd is None:
            st = dict(vars(args), display=self.state.get("screen") or args.display)
            self.lcd = ST7789.from_settings(st, rotate180=self.state["screen_flip"])
            W, H = self.lcd.WIDTH, self.lcd.HEIGHT
            self.splash()
        else:
            self.lcd = lcd                      # splash is already on screen
            self.lcd.rotate180 = self.state["screen_flip"]
        W, H = self.lcd.WIDTH, self.lcd.HEIGHT
        log.info("Display: %s, %dx%d", self.lcd.profile, W, H)

        self.events = queue.Queue()
        pins = {name: getattr(args, f"pin_{name}", None) or pin for name, pin in PINS.items()}
        self.controls = Controls(self.events, flip=self.state["screen_flip"], pins=pins)
        if progress:
            progress(0.75)
        loader = camera_loader or CameraLoader(fake=args.no_camera, swap_rb=args.swap_rb)
        self.cam, err = loader.result()           # usually ready by now
        try:                                      # back to normal priority (see zeroichi.py)
            os.setpriority(os.PRIO_PROCESS, threading.get_native_id(), 0)
        except (OSError, AttributeError):
            pass
        t = getattr(self.cam, "timing", {}) if self.cam else {}
        log.info("Camera ready (loaded in %.1f s in the background: %s)", loader.seconds,
                 ", ".join(f"{k} {v:.1f}s" for k, v in t.items()) or "-")
        self.cam_retry = None
        if err is not None or self.cam is None:
            # Never show a fake picture: say so, and keep trying in the background.
            log.error("Camera not found yet: %s - still trying", err)
            self.cam = NoCamera(self.preview_size_for_rotation())
            self.start_camera_retry()
        self.apply_camera_orientation()                # no-op if already right
        if progress:
            progress(1.0)
        self.cam.set_ev(self.state["ev"])

        self.screen = "camera"          # camera | menu | gallery | confirm
        self.menu_idx = self.menu_top = 0
        self.slots = {"presets": "None", "about": "", "photospace": "", "erase": "", "poweroff": ""}
        self.menu_rows = list(MENU)          # (Photo space: only while it's needed - see open_menu)
        self.presets_open, self.preset_idx = False, 0
        self._presets, self._preset_confirm = [None] * NPRESETS, (None, 0.0)
        self.space_offer = None
        self.toast = None                   # (text, until) short message on screen
        self.view = (0, 0, W, H)            # where the picture sits on the screen
        self._fit_cache = {}
        self._canvas = {}
        self._lv_perf = [0, 0.0, 0.0, 0.0, time.monotonic(), 0.0, 0.0, 0.0]
        self._cpu = CpuMeter()
        self.asleep = 0                           # 1 = screen off, 2 = the screen saver (v51)
        self.last_input = time.monotonic()        # the last key press (or the end of a recording)
        self._ss_t0 = self._ss_next = 0.0
        self._ss_art = None
        self._free, self._free_at = None, 0.0
        self.choose_photo_dir()
        self.saver = Saver()
        self.developer = Developer()
        self.developer.hold = lambda: bool(self.rec)    # pause developing while recording
        try:                                    # videos still to develop from last time
            for half in self.photo_dir.glob("VID_*.mov.dev"):  # half-written results
                half.unlink(missing_ok=True)
            for raw in sorted(self.photo_dir.glob("VID_*.part")):
                if raw.with_name(raw.name + ".json").exists():
                    log.info("Resuming developing %s", raw.name)
                    self.developer.submit(raw)
                else:
                    raw.unlink(missing_ok=True)
        except OSError:
            pass
        self._reserved = set()                 # file names of photos still being saved
        self._next_num = None                  # next file number (see new_media_path)
        self.rec = None                 # active GIF / video recording
        self.media, self.gidx, self.gal = [], 0, None
        self.count = len(self.list_media())
        self._ov_cache = {}
        self._glyphs = {}
        self._row2_right = 0
        self._dirty_at = None
        self._last_saved = None
        self.running = True
        self._cleaned = False
        self._clock_at = time.monotonic()

    # -- settings that survive a power-off -------------------------------
    @staticmethod
    def merge_state(st, saved):
        """Copy valid values from `saved` (e.g. a JSON file) into state `st`."""
        names = [n for n, _ in filters.FILTERS]
        if saved.get("filter_name") in ("16-Bit", "8-Bit"):      # v33 and older: palette was
            pal = saved.get("pixel_palette")                      # a menu setting
            if f"16-Bit {pal}" in filters.PIXEL_LOOKS:
                saved = dict(saved, filter_name=f"16-Bit {pal}")
        if "filter_name" not in saved and isinstance(saved.get("filter"), int):
            old = filters.OLD_ORDER                     # older versions: list position
            name = old[saved["filter"]] if 0 <= saved["filter"] < len(old) else "Normal"
            saved = dict(saved, filter_name=filters.OLD_NAMES.get(name, name))
            saved.pop("filter")
        elif saved.get("filter_name") in filters.OLD_NAMES:
            saved = dict(saved, filter_name=filters.OLD_NAMES[saved["filter_name"]])
        for k, v in saved.items():
            if k == "ev" and isinstance(v, (int, float)):
                st[k] = min(EV_MAX, max(EV_MIN, float(v)))
            elif k == "filter_name" and v in names:
                st["filter"] = names.index(v)
            elif k in st and type(v) is type(st[k]):
                st[k] = v
        st["clip_size"] = camera_sizes.OLD_CLIP_NAMES.get(st.get("clip_size"), st.get("clip_size"))
        if isinstance(st.get("video_max"), int) and (st["video_max"] <= 0 or st["video_max"] > 720):
            st["video_max"] = 720                       # (older versions: longer / no limit)
        for key, _, choices, _ in MENU:
            if key not in ACTIONS and key != "screen" and st[key] not in choices:
                st[key] = DEFAULT_STATE[key]
        if isinstance(st.get("screens"), dict):          # each Zero Ichi's own screen (by Pi serial)
            st["screens"] = {str(k): config.display_canon(v) for k, v in st["screens"].items()
                             if isinstance(v, str) and config.display_canon(v) in config.DISPLAYS}
        if st.get("screen"):
            st["screen"] = config.display_canon(st["screen"])
        if st.get("screen") not in ("",) + tuple(config.DISPLAYS):
            st["screen"] = ""
        if st["mode"] not in MODES:
            st["mode"] = "PHOTO"
        st["filter"] %= len(filters.FILTERS)
        return st

    def load_state(self):
        """Your settings from last time (state.json in the zeroichi folder on the
        SD card - they are kept nowhere else)."""
        st = dict(DEFAULT_STATE, screen_flip=self.args.rotate == 180)
        try:
            saved = json.loads(STATE_FILE.read_text())
            if isinstance(saved, dict):
                saved.pop("_saved", None)
                self.merge_state(st, saved)
        except (OSError, ValueError, AttributeError):
            pass
        return st

    def screen_profiles(self):
        """Which display each Screen menu choice means: the 320x240 type from
        settings.txt (the 2.8inch hat unless set otherwise), or the 1.3inch HAT."""
        base = self.args.display
        p320 = base if base in ("2.8inch-hat", "st7789-320x240", "ili9341-320x240") \
            else "2.8inch-hat"
        return {"320x240": p320, "240x240": "1.3inch-hat"}

    def switch_screen(self, choice):
        """Change the screen type right away (both hats use the same pins)."""
        global W, H
        profile = self.screen_profiles()[choice]
        if profile == self.lcd.profile:
            return
        self.lcd.wait()
        self.lcd.release()
        st = dict(vars(self.args), display=profile)
        self.lcd = ST7789.from_settings(st, rotate180=self.state["screen_flip"])
        W, H = self.lcd.WIDTH, self.lcd.HEIGHT
        self._ov_cache.clear()
        self._canvas.clear()
        self._fit_cache.clear()
        self.view = (0, 0, W, H)
        self.state["screen"] = profile
        serial = config.pi_serial()
        if serial:                                 # (this Zero Ichi's screen, from now on)
            self.state["screens"] = dict(self.state.get("screens") or {}, **{serial: profile})
        self.save_state()
        self.apply_camera_orientation()            # live view size follows the screen
        log.info("Screen switched to %s (%dx%d)", profile, W, H)

    # -- where photos go: the PHOTOS partition (whole card) or bootfs --------
    def choose_photo_dir(self):
        """photo_dir "auto": the PHOTOS partition when the card has one (waits a
        little for it to be ready at start-up), else bootfs/photos. Photos still
        on bootfs are moved over to PHOTOS once."""
        if self._photo_setting not in ("auto", str(BOOT_PHOTOS)):
            return
        def expected():        # (in fstab, or being formatted right now by the fast start)
            return _fstab_has(PHOTOS_MOUNT) or (
                fast_start_running() and (ZEROICHI_STATE / "space-format").exists())

        if not os.path.ismount(PHOTOS_MOUNT) and expected():
            t = time.monotonic()
            while (not os.path.ismount(PHOTOS_MOUNT) and expected()
                   and time.monotonic() - t < 90):
                if time.monotonic() - t > 1.5:
                    self.message("Opening the PHOTOS drive", "one moment...")
                time.sleep(0.25)
        if os.path.ismount(PHOTOS_MOUNT):
            self.photo_dir = PHOTOS_MOUNT
            self.move_old_photos()
        else:
            self.photo_dir = BOOT_PHOTOS
            if _fstab_has(PHOTOS_MOUNT):
                log.warning("PHOTOS partition not mounted - saving to %s", BOOT_PHOTOS)
        try:
            self.photo_dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
        self.report_space_log()

    def move_old_photos(self):
        """Move photos, GIFs and videos from bootfs/photos to the PHOTOS drive
        (once, after the card got its PHOTOS partition)."""
        import shutil
        try:
            files = sorted(p for p in BOOT_PHOTOS.iterdir() if p.is_file())
        except OSError:
            return
        if not files:
            return
        total = sum(p.stat().st_size for p in files)
        log.info("Moving %d files (%s) from %s to the PHOTOS drive", len(files), gb(total),
                 BOOT_PHOTOS)
        done = 0
        for i, p in enumerate(files):
            if i % 3 == 0:
                self.message("Moving your photos", f"to the PHOTOS drive: {i + 1} of {len(files)}")
            dst = PHOTOS_MOUNT / p.name
            try:
                if dst.exists() and dst.stat().st_size == p.stat().st_size:
                    p.unlink()
                    continue
                tmp = dst.with_name("." + dst.name + ".moving")
                shutil.copy2(p, tmp)
                os.replace(tmp, dst)
                p.unlink()
                done += 1
            except OSError as e:
                log.warning("Could not move %s: %s", p.name, e)
        os.sync()
        try:
            BOOT_PHOTOS.rmdir()
        except OSError:
            pass
        log.info("Moved %d files to the PHOTOS drive", done)
        self.show_toast("Photos moved to the PHOTOS drive", 3)

    def report_space_log(self):
        """After "make room for photos": put its report into the log and say how
        it went."""
        f = ZEROICHI_STATE / "space-log.txt"
        try:
            text = f.read_text(errors="replace")
            f.unlink()
        except OSError:
            return
        lines = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith(
            ("Pass ", "Partition #", "Re-reading")) and " records " not in ln]
        log.info("Make room for photos:\n  %s", "\n  ".join(lines[-60:]))
        try:
            if os.path.ismount(PHOTOS_MOUNT):
                st = os.statvfs(PHOTOS_MOUNT)
                self.show_toast(f"PHOTOS drive ready\n{gb(st.f_blocks * st.f_frsize)}", 4)
                return
            if whole_card():
                st = os.statvfs(BOOT_PHOTOS.parent)
                self.show_toast(f"Whole card ready\n{gb(st.f_bavail * st.f_frsize)} for photos", 4)
                return
        except OSError:
            pass
        self.show_toast("Photo space: didn't work\n(see zeroichi-log.txt)", 4)

    def photospace(self, _slot=None):            # (MENU > Photo space)
        self.photo_space_action()

    def poweroff(self, _slot=None):              # (MENU > Power off)
        self.screen = "camera"
        self.power_off()

    def maybe_offer_space(self):
        """At start-up the card gets its room by itself, after a short countdown -
        unless settings.txt says photo_space = menu, or two tries didn't work."""
        if self._photo_setting not in ("auto", str(BOOT_PHOTOS)):
            return
        if self._space_setting == "menu" or space_fails() >= 2:
            return
        if Path("/run/zeroichi/space-tried").exists():
            return
        plan, why = space_plan()
        if plan is not None:
            log.info("Photo space: starting by itself in %d s (%s card)", SPACE_AUTO_SECONDS,
                     "small" if plan["MODE"] == "photos" else "one-drive")
            self.space_offer = plan
            self._space_drawn = None
            self._space_deadline = time.monotonic() + SPACE_AUTO_SECONDS
            self.screen = "space"

    def photo_space_action(self, _slot=None):
        """MENU > Photo space: offer to use the whole card for photos."""
        plan, why = space_plan()
        if plan is None:
            if why == "done":
                try:
                    st = os.statvfs(PHOTOS_MOUNT if os.path.ismount(PHOTOS_MOUNT) else BOOT_PHOTOS.parent)
                    self.show_toast(f"Whole card in use\n{gb(st.f_bavail * st.f_frsize)} free", 3)
                except OSError:
                    self.show_toast("Whole card in use", 2)
            else:
                log.info("Photo space: not possible (%s)", why)
                self.show_toast(f"Not possible:\n{why}", 4)
            return
        self.space_offer = plan
        self._space_drawn = None
        self._space_deadline = None
        self.screen = "space"

    def space_overlay_text(self, left=None):
        plan = self.space_offer
        lines = ["Use the whole card", "for photos", "", f"Takes about {plan['minutes']} minutes.",
                 "Your photos are kept.", "Don't switch off meanwhile.", ""]
        if left is None:
            return lines + ["KEY1 = yes   KEY2 = not now"]
        return lines + [f"Starting in {left} s", "KEY1 = start  KEY2 = later"]

    def tick_space(self):
        left = None
        if getattr(self, "_space_deadline", None):
            left = math.ceil(self._space_deadline - time.monotonic())
            if left <= 0:
                self.make_space()
                return
        key = ("space", W, H, left)
        if getattr(self, "_space_drawn", None) == key:
            return
        self._space_drawn = key
        self.lcd.show_pil(self.lines_image(self.space_overlay_text(left), (255, 220, 80)))

    def lines_image(self, lines, title_color=(255, 255, 255), bg=(20, 20, 28)):
        """A screen with a title (first lines up to an empty one) and text."""
        img = Image.new("RGB", (W, H), bg)
        d = ImageDraw.Draw(img)
        title = []
        for ln in lines:
            if not ln:
                break
            title.append(ln)
        rest = lines[len(title):]
        y = 26
        for ln in title:
            self.text(d, (W // 2, y), ln, self.f18 if W < 300 else self.f22, fill=title_color)
            y += 26 if W < 300 else 28
        y += 6
        for ln in rest:
            if ln:
                self.text(d, (W // 2, y), ln, self.f14 if W < 300 else self.f16,
                          fill=(255, 220, 80) if ln.startswith("KEY") else (220, 220, 230))
            y += 21
        return img

    def save_screen_message(self, img, path=None):
        """Keep a picture for the fast start to put back on the screen after
        Zero Ichi has stopped (see fast-start.sh: show)."""
        try:
            a = np.asarray(img.convert("RGB"))
            run = Path("/run/zeroichi")
            default = (run if run.is_dir() else ZEROICHI_STATE) / "message.rgb565"
            (path or default).write_bytes(filters.rgb565(a).tobytes())
        except OSError as e:
            log.warning("Could not save the screen message: %s", e)

    def make_step_screens(self, plan):
        """The screens the fast start shows while it works, one per step."""
        steps = {"move": [("check", "checking the card"), ("shrink", "making the system smaller"),
                          ("move", "moving the system"), ("drive", "making the ZEROICHI drive")],
                 "fat": [("drive", "making the ZEROICHI drive")],
                 "photos": [("check", "checking the card"), ("shrink", "making the system smaller"),
                            ("parts", "adding the PHOTOS drive")]}[plan["MODE"]]
        d = ZEROICHI_STATE / "steps"
        d.mkdir(parents=True, exist_ok=True)
        first = None
        for i, (name, what) in enumerate(steps):
            img = self.lines_image(["Making room", "for photos", "",
                                    f"Step {i + 1} of {len(steps)}:" if len(steps) > 1 else "One moment:",
                                    what, "", "Don't switch off!", "The green light blinks.",
                                    f"About {plan['minutes']} minutes in all."], (255, 220, 80))
            self.save_screen_message(img, d / f"{name}.rgb565")
            first = first or img
        return first

    def on_space(self, name):
        if name == "key1":
            self.make_space()
            return
        if name != "key2":
            return
        auto = getattr(self, "_space_deadline", None)
        self._space_deadline = None
        self.state["space_asked"] = True
        self.mark_dirty()
        self.screen = "camera"
        self._space_drawn = None
        self.show_toast("Later: at the next start\n(or MENU > Photo space)" if auto
                        else "Later: MENU >\nPhoto space", 2.5)

    def make_space(self):
        """Hand over to the fast start, which makes room for photos
        (fast-start.sh, make_space), then restarts."""
        self._space_deadline = None
        plan, why = space_plan()
        if plan is None:
            self.screen = "camera"
            self.show_toast(f"Not possible:\n{why}", 4)
            return
        if self.rec:
            self.stop_recording()
        self.wait_for_saves("Finishing saving...")
        if self.developer.pending:
            self.message("Finishing a video...", "one moment")
            t = time.monotonic()
            while self.developer.pending and time.monotonic() - t < 600:
                time.sleep(0.5)
                self.pump_saver()
        if plan["MODE"] == "photos":
            log.info("Making room for photos: system partition %s -> %s, PHOTOS drive %s (%s)",
                     plan["PART"], gb(plan["system"]), gb(plan["gain"]), plan)
        else:
            log.info("Making room for photos: system partition %s -> %s at the end of the card, "
                     "one ZEROICHI drive %s (%s)", plan["PART"], gb(plan["system"]), gb(plan["gain"]), plan)
        try:
            img = self.make_step_screens(plan)
            self.lcd.show_pil(img)
            self.save_screen_message(img)
            text = "".join(f"{k}={plan[k]}\n" for k in PLAN_KEYS if k in plan)
            tmp = ZEROICHI_STATE / "space-plan.tmp"
            tmp.write_text(text)
            os.replace(tmp, ZEROICHI_STATE / "space-plan")
        except OSError as e:
            log.error("Could not start making room: %s", e)
            self.screen = "camera"
            self.show_toast("Photo space failed", 2)
            return
        self.state["space_asked"] = True
        self.save_state()
        self.cleanup(keep_screen=True)
        self.remember_clock()
        logging.shutdown()
        os.sync()
        os._exit(EXIT_MAKE_SPACE)

    # -- standby: KEY3 held - screen and camera off, back in a second ----------
    def standby(self):
        if self.rec:
            self.stop_recording()
        self.save_state()
        self.asleep = 1
        self.lcd.sleep()
        if hasattr(self.cam, "pause"):
            self.cam.pause()
        set_cpu_speed("powersave")
        os.sync()
        log.info("Standby (KEY3 held)")

    def free_space(self, max_age=15.0):
        """Free bytes on the photo card (checked every few seconds), or None."""
        now = time.monotonic()
        if self._free_at <= 0 or now - self._free_at > max_age:     # (0 = not measured yet)
            try:
                st = os.statvfs(self.photo_dir)
                self._free = st.f_bavail * st.f_frsize
            except OSError:
                self._free = None
            self._free_at = now
        return self._free

    def card_full(self):
        free = self.free_space(max_age=5.0)
        return free is not None and free < MIN_SPACE

    # -- screen saver -----------------------------------------------------
    def saver_timeout(self):
        return SAVER_SECONDS.get(self.state.get("screensaver"), 60)

    def maybe_sleep(self):
        t, now = self.saver_timeout(), time.monotonic()
        # (v51) never while busy - recording, saving - and the wait starts again
        # when it's done (a 1-minute video with the 1-minute screen saver ended
        # in a dark screen, the saving still going on)
        if self.rec or self.saver.pending:
            self.last_input = now
            return
        if not t or self.asleep or now - self.last_input < t:
            return
        self.asleep = 2                           # the screen saver's picture (see SaverArt)
        self._ss_t0 = self._ss_next = now
        if hasattr(self.cam, "pause"):
            self.cam.pause()                      # no frames: the Pi is free for saving
        log.info("Screen saver on (%s without a key press)", self.state.get("screensaver"))
        self.saver_tick()

    def saver_tick(self):
        now = time.monotonic()
        if now - self._ss_t0 >= SAVER_DARK_AFTER:  # after 10 minutes: the screen off
            self.asleep, self._ss_art = 1, None
            self.lcd.wait()
            self.lcd.sleep()
            log.info("Screen saver: screen off (%d min of the screen saver)", SAVER_DARK_AFTER // 60)
            return
        if now < self._ss_next:
            return
        self._ss_next = max(self._ss_next + 1 / SAVER_FPS, now)
        try:
            if self._ss_art is None or (self._ss_art.w, self._ss_art.h) != (W, H):
                self._ss_art = SaverArt(W, H)
            self.lcd.show_pil(self._ss_art.frame(now - self._ss_t0))
        except Exception:
            log.exception("Screen saver picture failed")
            self._ss_next = now + 3600

    def wake_up(self):
        self.asleep, self._ss_art = 0, None
        self.last_input = time.monotonic()
        set_cpu_speed("performance")
        self.lcd.wake()
        if hasattr(self.cam, "resume"):
            self.cam.resume()
        try:                                      # a fresh picture before the light is on
            if self.screen in ("camera", "menu"):
                self.tick_camera()
                self.lcd.wait()
            elif self.screen == "gallery":
                self.show_item_frame()
            elif self.screen == "developing":
                self._dev_shown = 0.0
                self.tick_developing()
            elif self.screen == "about":
                self.show_about()
        except Exception:
            log.exception("Redraw after the screen saver failed")
        self.lcd.backlight_on()

    def about(self, _slot=None):
        """(v53) MENU > About: a page of its own (about_image) - any key: back"""
        self.screen = "about"
        self.show_about()

    def show_about(self):
        free = self.free_space(max_age=0)
        if free is None:
            fr = "-"
        elif free >= 10 << 30:
            fr = f"{free / 2 ** 30:.0f} GB"
        elif free >= 1 << 30:
            fr = f"{free / 2 ** 30:.1f} GB"
        else:
            fr = f"{free / 2 ** 20:.0f} MB"
        rows = [("Version", f"v{config.VERSION}"),
                ("Screen", f"{W}x{H}"),
                ("Start-up time", f"{self.ready_after:.1f} s" if self.ready_after else "-"),
                ("Photos, GIFs & videos (Photos & videos)", str(len(self.list_media()))),
                ("Free space", fr)]
        self.lcd.show_pil(about_image(rows, self.lcd.profile))

    def on_about(self, name):
        self.screen = "menu"

    # -- (0.1.0) MENU > Erase all: every photo, GIF and video - asked twice --
    # Only Zero Ichi's own files in the photos folder: settings, presets and
    # Zero Ichi itself stay (not a "format" - the drive holds those too). The
    # numbers carry on counting up, so new files never get an old one's name.
    def erase(self, _slot=None):
        if self.developer.pending:
            self.show_toast("Developing a video -\ntry again in a moment", 2.5)
            return
        self.wait_for_saves()
        files = self.list_media()
        self.count = len(files)
        if not files:
            self.show_toast("Nothing to erase", 1.5)
            return
        self._erase_step = 1
        self.screen = "erase"
        self._erase_question()

    def _erase_question(self):
        n = self.count
        if self._erase_step == 1:
            lines = ["Erase all files?", "", "1 file" if n == 1 else f"{n} photos, GIFs and videos",
                     "KEY3 = yes   other = no"]
        else:
            lines = ["Are you sure?", "", "They can't be brought back.", "KEY3 = erase   other = no"]
        self.lcd.show_pil(brand_image(lines, ZI_RED))

    def on_erase(self, name):
        if name == "key3" and self._erase_step == 1:
            self._erase_step = 2
            self._erase_question()
            return
        if name == "key3" and self._erase_step == 2:
            self._erase_now()
        self._erase_step = 0
        self.slots["erase"] = "1 file" if self.count == 1 else f"{self.count} files"
        self.screen = "menu"

    def _erase_now(self):
        files = self.list_media()
        done, t0, shown = 0, time.monotonic(), 0.0
        for i, p in enumerate(files):
            try:
                p.unlink()
                done += 1
            except OSError:
                pass
            if time.monotonic() - shown > 0.3:
                self.message("Erasing...", f"{i + 1} of {len(files)}")
                shown = time.monotonic()
        os.sync()
        self.count = len(self.list_media())
        self.gidx = 0
        self._free_at = 0                               # (measured again)
        log.info("Erase all: %d of %d photos, GIFs and videos erased in %.1f s",
                 done, len(files), time.monotonic() - t0)
        if done == len(files):
            self.message("Erased", "1 file" if done == 1 else "all photos, GIFs and videos")
        else:
            self.message("Erase failed", f"{len(files) - done} could not be erased", bg=(90, 10, 10))
        time.sleep(1.5)
        self.drain_events()

    def show_toast(self, text, seconds=1.5):
        self.toast = (text, time.monotonic() + seconds)

    def save_state(self):
        """Save every setting right away - to the SD card (only there) - and
        flush it, so it survives pulling the plug."""
        self._dirty_at = None
        if self.state == self._last_saved:
            return
        data = dict(self.state, _saved=time.time(),
                    filter_name=filters.FILTERS[self.state["filter"]][0])
        data.pop("filter")
        for path in (STATE_FILE,):
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                tmp = path.with_suffix(".tmp")
                with open(tmp, "w") as f:
                    json.dump(data, f, indent=1)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(tmp, path)
            except OSError as e:
                log.warning("Could not save settings to %s: %s", path, e)
        os.sync()                  # FAT keeps file changes in memory ~30 s otherwise
        self._last_saved = dict(self.state)

    def mark_dirty(self):
        self._dirty_at = time.monotonic()

    def apply_setting(self, key):
        if key in ("cam_rotate", "mirror", "size", "clip_size", "mode"):
            self.apply_camera_orientation()
        if key == "screen_flip":
            self.lcd.rotate180 = self.state["screen_flip"]
            self.controls.flip = self.state["screen_flip"]

    # -- drawing helpers -------------------------------------------------
    def text(self, d, xy, s, font, fill="white", anchor="mm", stroke=2):
        d.text(xy, s, font=font, fill=fill, anchor=anchor,
               stroke_width=stroke, stroke_fill="black")

    def overlay(self, draw_fn):
        """Draw onto a transparent layer. Opaque pixels replace the picture;
        half-transparent ones (alpha 40-127) darken it."""
        img = Image.new("RGBA", (W, H), (0, 0, 0, 0))
        draw_fn(ImageDraw.Draw(img))
        a = np.asarray(img)
        alpha = a[..., 3].reshape(-1)
        solid = np.flatnonzero(alpha > 127)             # only the pixels that change
        dim = np.flatnonzero((alpha > 40) & (alpha <= 127))
        return self._ov(solid, a[..., :3].reshape(-1, 3)[solid], dim if dim.size else None)

    @staticmethod
    def _ov(pixels, colours, dim=None):
        return Overlay(pixels, colours, dim)

    def overlay_box(self, x, y, w, h, draw_fn):
        """Like overlay(), but only draws a small w x h box at (x, y) - much
        cheaper to rebuild, e.g. for the recording timer that changes often."""
        x, y = max(0, x), max(0, y)
        w, h = min(w, W - x), min(h, H - y)
        img = Image.new("RGBA", (w, h), (0, 0, 0, 0))
        draw_fn(ImageDraw.Draw(img))
        a = np.asarray(img)
        rows, cols = np.nonzero(a[..., 3] > 127)
        solid = (rows + y) * W + (cols + x)
        return self._ov(solid, a[rows, cols, :3])

    def _glyph(self, ch, font, fill):
        """One character with its black outline, drawn once and kept: (outline
        rows, cols, letter rows, cols, advance)."""
        key = (ch, id(font), fill)
        g = self._glyphs.get(key)
        if g is None:
            asc, desc = font.getmetrics()
            adv = font.getlength(ch)
            w, h = int(adv) + 10, asc + desc + 10
            im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
            ImageDraw.Draw(im).text((5, 5), ch, font=font, fill=fill, stroke_width=2,
                                    stroke_fill="black")
            body = Image.new("L", (w, h), 0)
            ImageDraw.Draw(body).text((5, 5), ch, font=font, fill=255)
            fm = np.asarray(body) > 127
            sm = (np.asarray(im)[..., 3] > 127) & ~fm
            ys, xs = np.nonzero(sm)
            yf, xf = np.nonzero(fm)
            g = self._glyphs[key] = (ys - 5, xs - 5, yf - 5, xf - 5, adv)
        return g

    def text_overlay(self, xy, text, font, fill="white", anchor="lm"):
        """Outlined text as an overlay, put together from kept characters - many
        times quicker than drawing it, for text that changes often (the
        recording timer, the space left on the card)."""
        rgb = ImageColor.getrgb(fill) if isinstance(fill, str) else tuple(fill)[:3]
        glyphs = [self._glyph(ch, font, rgb) for ch in text]
        total = sum(g[4] for g in glyphs)
        asc, desc = font.getmetrics()
        x = xy[0] - {"l": 0, "m": total / 2, "r": total}[anchor[0]]
        y = int(round(xy[1] - (asc + desc) / 2))
        outline, letters = [], []
        for g in glyphs:
            ox = int(round(x))
            outline.append(((g[0] + y), (g[1] + ox)))
            letters.append(((g[2] + y), (g[3] + ox)))
            x += g[4]

        def px(parts):
            if not parts:
                return np.zeros(0, np.intp)
            ys = np.concatenate([p[0] for p in parts])
            xs = np.concatenate([p[1] for p in parts])
            keep = (ys >= 0) & (ys < H) & (xs >= 0) & (xs < W)
            return ys[keep] * W + xs[keep]
        f = px(letters)
        o = px(outline)
        on_letter = np.zeros(H * W, bool)
        on_letter[f] = True
        o = o[~on_letter[o]]                             # the letters stay on top
        colours = np.zeros((o.size + f.size, 3), np.uint8)
        colours[o.size:] = rgb
        return self._ov(np.concatenate([o, f]), colours)

    @staticmethod
    def compose(frame, ov):
        """Draw an overlay onto a full-screen frame (touches only its pixels):
        an R,G,B frame (H x W x 3) or a 16-bit screen frame (H x W x 2)."""
        if not (frame.flags.writeable and frame.flags.c_contiguous):
            frame = np.array(frame)                     # writeable, contiguous copy
        if frame.shape[-1] == 2:
            values, where, dim = ov.words()
            flat = frame.view(np.uint16).reshape(-1)
            if dim is not None:
                flat[dim] = dim565(flat[dim])
        else:
            values, where, dim = ov.bytes3()
            flat = frame.reshape(-1)
            if dim is not None:
                flat[dim] //= 3
        flat[where] = values
        return frame

    def cached(self, key, draw_fn):
        hit = self._ov_cache.get(key[0])
        if hit and hit[0] == key:
            return hit[1]
        ov = self.overlay(draw_fn)
        self._ov_cache[key[0]] = (key, ov)
        return ov

    def splash(self):
        """The start-up picture (v51: Zero Ichi's - the file the fast start shows;
        if it's missing, a plain version of the same design)."""
        here = Path(__file__).resolve().parent
        name = "splash.rgb565" if W == 240 else f"splash-{W}x{H}.rgb565"
        try:
            raw = (here / name).read_bytes()
            if len(raw) == W * H * 2:
                self.lcd.show_bytes(raw)
                return
        except OSError:
            pass
        cream, red, yel, blu, blk = (242, 234, 216), (220, 58, 40), (246, 184, 26), (28, 76, 156), (24, 24, 26)
        img = Image.new("RGB", (W, H), cream)
        d = ImageDraw.Draw(img)
        wide = W > H
        s = .92 if wide else .78
        x, y, r = W * (.36 if wide else .34), H * (.38 if wide else .36), 60 * s
        d.ellipse((x - r, y - r, x + r, y + r), fill=red)                       # 0
        d.ellipse((x - .4 * r, y - .4 * r, x + .4 * r, y + .4 * r), fill=cream)
        bx = x + r + 22 * s                                                     # 1
        d.rectangle((bx, y - r, bx + 30 * s, y + r), fill=blk)
        d.polygon([(bx, y - r), (bx, y - r + 30 * s), (bx - 26 * s, y - r + 30 * s)], fill=blk)
        q = 42 * s
        d.pieslice((bx + 30 * s - q, y + r - q, bx + 30 * s + q, y + r + q), 270, 360, fill=yel)
        d.rectangle((bx + 46 * s, y + r * .24, bx + 76 * s, y + r * .24 + 30 * s), fill=blu)
        d.text((W * .125, H * .83), "zero ichi", font=self.f30 if wide else self.f22, fill=blk, anchor="lm")
        d.text((W * .875, H * .94), f"v{config.VERSION} \u2022 {W}x{H}", font=load_font(12),
               fill=(120, 112, 98), anchor="rm")
        self.lcd.show_pil(img)

    def message(self, title, sub=None, bg=(20, 20, 28)):
        """(v52) In Zero Ichi's design (brand_image) - a red title: a warning."""
        warn = bg[0] > bg[1] + 40
        self.lcd.show_pil(brand_image([title, "", sub] if sub else [title], ZI_RED if warn else ZI_INK))

    def hud(self):
        st = self.state
        name = filters.FILTERS[st["filter"]][0]
        rec = self.rec_label() is not None          # the timer itself: rec_overlay()
        saving = self.saver.pending > 0
        dev = self.developer.progress
        dev = None if dev is None else int(dev * 100) // 5 * 5     # rebuild every 5 %
        look = name
        key = ("hud", name, st["ev"], self.count, st["mode"], rec,
               self.aspect(), self.view, saving, st["clip_size"], dev)
        hit = self._ov_cache.get("hud")
        if hit and hit[0] == key:
            return hit[1]
        # layout: what goes where along the bottom (the space left: space_overlay)
        label = ex = None
        right2 = None                                   # right end of the second row
        if not rec:
            label = st["mode"]
            if st["mode"] == "VIDEO" and self.instant_video(look):
                cw, ch = self.instant_size(look)        # instant videos: their own size
                label += f" {cw}x{ch}"
            elif st["mode"] != "PHOTO":
                cw, ch = self.target_size(kind="clip")
                label += f" {cw}x{ch}"
            elif self.aspect() != W / H:                # picture shape differs from screen
                label += " 6x6" if self.is_square_filter() else (
                    " 4:3" if self.aspect() > 1 else " 1:1")
            extra = []
            if st["ev"]:
                extra.append(f"EV{st['ev']:+.1f}")
            if extra:
                ex = " ".join(extra)
        cw = self.f14.getlength(f"#{self.count}")
        ex_fits = ex is not None and (self.f14.getlength(label) + self.f14.getlength(ex)
                                      + cw + 34) <= W
        if dev is not None:
            right2 = f"DEVELOPING {dev}%"
        elif saving:
            right2 = "SAVING"
        elif ex is not None and not ex_fits:            # small screen: one row up
            right2 = ex
        self._row2_right = self.f14.getlength(right2) if right2 else 0

        def draw(d):
            self.text(d, (W // 2, 14), f"<  {name}  >", self.f18)
            if label is not None:
                self.text(d, (6, H - 14), label, self.f14,
                          fill=MODE_COLORS[st["mode"]], anchor="lm")
            if ex is not None and ex_fits:              # right-aligned, next to the counter
                self.text(d, (int(W - 6 - cw - 10), H - 14), ex, self.f14,
                          fill=(255, 220, 80), anchor="rm")
            if right2:
                self.text(d, (W - 6, H - 32), right2, self.f14, fill=(255, 220, 80),
                          anchor="rm")
            self.text(d, (W - 6, H - 14), f"#{self.count}", self.f14, anchor="rm")

        return self.cached(key, draw)

    def space_overlay(self):
        """How much still fits on the card, for the current mode (bottom left,
        second row): "1234 photos left", "56 GIFs left", "1h05m left"."""
        st = self.state
        k = (self.free_space(), st["mode"], st["filter"], st["clip_size"], st["gif_frames"],
             st["size"], st["cam_rotate"], st["mirror"], self._row2_right, id(st.get("space")),
             W, H)
        hit = self._ov_cache.get("space")
        if hit and hit[0] == k:
            return hit[1]
        long_t, short_t, low = self.space_text()
        ov = None
        if long_t:
            room = W - 12 - (self._row2_right + 12 if self._row2_right else 0)
            text = long_t if self.f14.getlength(long_t) <= room else short_t
            ov = self.text_overlay((6, H - 32), text, self.f14,
                                   fill=(255, 90, 90) if low else (205, 205, 215))
        self._ov_cache["space"] = (k, ov)
        return ov

    def space_left(self):
        """What still fits on the card in the current mode: ("photos", n),
        ("GIFs", n) or ("video", seconds) - from the free space and the size of
        your recent files with this look (or a typical size). None: unknown."""
        free = self.free_space()
        if free is None:
            return None
        usable = max(0, free - MIN_SPACE)
        mode = self.state["mode"]
        kind = mode.lower()
        look = filters.FILTERS[self.state["filter"]][0]
        learned = (self.state.get("space") or {}).get(f"{kind}:{look}")
        bpp = learned if isinstance(learned, (int, float)) and learned > 0 else \
            SPACE_DEFAULT[kind] * SPACE_LOOK.get(look, 1.0)
        if mode == "PHOTO":
            w, h = self.target_size(photo=True)
            return "photos", int(usable // max(1.0, bpp * w * h))
        w, h = self.target_size(kind="clip")
        if mode == "GIF":
            return "GIFs", int(usable // max(1.0, bpp * w * h * self.state["gif_frames"]))
        if self.instant_video(look):
            w, h = self.instant_size(look)
        return "video", int(usable // max(1.0, bpp * w * h * VIDEO_FPS))

    def space_text(self):
        """(long text, short text, low?) for the space left on the card."""
        r = self.space_left()
        if r is None:
            return "", "", False
        free = self._free
        if free < MIN_SPACE:
            return "Card full", "Card full", True
        what, n = r
        low = free < LOW_SPACE
        if what == "video":
            m = n // 60
            t = f"{m // 60}h{m % 60:02d}m" if m >= 60 else (f"{m} min" if m >= 1 else f"{n} s")
            return f"{t} left", t, low
        num = f"{n // 1000}k" if n >= 10000 else str(n)
        return f"{num} {what} left", f"{num} left", low

    def learn_space(self, kind, look, pixels, nbytes):
        """Remember how many bytes per pixel this kind of file took (averaged),
        so "what's left" matches your own photos, GIFs and videos."""
        if not pixels or not nbytes or pixels <= 0 or nbytes <= 0:
            return
        sp = dict(self.state.get("space") or {})       # a new dict: settings get saved
        k = f"{kind}:{look}"
        v = nbytes / pixels
        old = sp.get(k)
        sp[k] = round(v if not isinstance(old, (int, float)) else old * 0.7 + v * 0.3, 4)
        if len(sp) > 40:
            sp = dict(list(sp.items())[-40:])
        self.state["space"] = sp
        self._free_at = 0                              # free space changed too
        self.mark_dirty()

    def rec_label(self):
        if not self.rec or self.rec.get("kind") not in ("gif", "video"):
            return None
        if self.rec["kind"] == "video":
            s = int(time.monotonic() - self.rec["start"])
            m = self.state["video_max"]
            if not m:
                return f"\u25cf REC {s // 60}:{s % 60:02d}"
            limit = f"{m}s" if m < 60 else f"{m // 60}:{m % 60:02d}"
            return f"\u25cf REC {s // 60}:{s % 60:02d} / {limit}"
        return "\u25cf GIF"          # fixed text (progress = the red bar): no redraw per frame

    def rec_overlay(self):
        """The recording timer / GIF counter, in its own small box."""
        text = self.rec_label()
        key = ("rec", text, W, H)
        hit = self._ov_cache.get("rec")
        if hit and hit[0] == key:
            return hit[1]
        ov = self.text_overlay((6, H - 16), text, self.f14, fill=REC_RED)   # (changes every second)
        self._ov_cache["rec"] = (key, ov)
        return ov

    def menu_value(self, key):
        if key in ACTIONS:
            return self.slots[key]
        if key == "screen":
            return f"{W}x{H}"
        return self.state[key]

    def open_menu(self):
        self.slots["erase"] = "1 file" if self.count == 1 else f"{self.count} files"
        self.menu_rows = [r for r in MENU if r[0] != "photospace" or photo_space_needed()]
        self.menu_idx %= len(self.menu_rows)
        self.presets_open = False
        self.presets_refresh()
        self.screen = "menu"

    # -- MENU > Presets: a panel inside the menu ----------------------------
    def presets_refresh(self):
        self._presets = [preset_read(self.state, preset_path(i)) for i in range(NPRESETS)]
        for p in self._presets:
            if p:
                p.pop("_screen", None)
                p.pop("_camera", None)
        n = sum(p is not None for p in self._presets)
        self.slots["presets"] = f"{n} saved" if n else "None"
        self._preset_confirm = (None, 0.0)

    def presets(self, _slot=None):              # (MENU > Presets)
        self.presets_open = True
        self.presets_refresh()

    def presets_overlay(self):
        rows = tuple("(empty)" if p is None else preset_summary(p) for p in self._presets)
        confirm = self._preset_confirm[0] == self.preset_idx and time.monotonic() < self._preset_confirm[1]
        key = ("menu", "presets", self.preset_idx, rows, confirm)

        def draw(d):                                # (v52: Zero Ichi's design - see ui_card)
            ui_card(d, "presets")
            f = brand_font(14)
            y = 52
            img = getattr(d, "_image", None)
            for i, r in enumerate(rows):
                while d.textlength(r, font=f) > W - 66 and len(r) > 4:
                    r = r[:-4] + "\u2026"
                chosen = i == self.preset_idx
                if chosen:
                    d.rectangle((10, y - 10, W - 11, y + 10), fill=ZI_INK + (255,))
                    if img is not None:
                        img.alpha_composite(ui_art("dot", W, H), (12, y - 5))
                d.text((26, y + 0.5), str(i + 1), font=f, fill=(ZI_YEL if chosen else ZI_BLU) + (255,), anchor="lm")
                col = ZI_CREAM if chosen else ZI_INK if self._presets[i] else ZI_GREY
                d.text((44, y + 0.5), r, font=f, fill=col + (255,), anchor="lm")
                y += 24
            l1 = "KEY2 again: replace it" if confirm else "KEY1: load    KEY2: save"
            d.text((14, H - 36), l1, font=brand_font(12), fill=(ZI_RED if confirm else ZI_INK) + (255,), anchor="lm")
            ui_hint(d, "PRESS: back")

        return self.cached(key, draw)

    def on_presets(self, name):
        i = self.preset_idx
        if name in ("up", "down"):
            self.preset_idx = (i + (1 if name == "down" else -1)) % NPRESETS
            self._preset_confirm = (None, 0.0)
        elif name == "key1":
            st = preset_read(self.state, preset_path(i))
            if st is None:
                self.show_toast("This preset is empty\n(KEY2 saves one)", 2)
                return
            screen = st.pop("_screen", None)
            camera = st.pop("_camera", "")
            me = config.pi_serial()
            if screen and me and camera != me and (st.get("screens") or {}).get(me):
                if screen != f"{W}x{H}":                    # (the screen belongs to the Zero Ichi)
                    log.info("Preset %d: its screen (%s) is another Zero Ichi's - this one keeps %dx%d",
                             i + 1, screen, W, H)
                screen = None
            old, self.state = self.state, st
            self.apply_setting("mode")                  # (camera set-up: rotation, size, mirror, clip size)
            if st["screen_flip"] != old["screen_flip"]:
                self.apply_setting("screen_flip")
            self.cam.set_ev(st["ev"])
            if screen and screen != f"{W}x{H}":
                self.switch_screen(screen)
            self.mark_dirty()
            log.info("Preset %d loaded (%s)", i + 1, preset_summary(st))
            self.presets_open = False
            self.screen = "camera"
            self.show_toast(f"Preset {i + 1} loaded", 2)
        elif name == "key2":
            c_slot, c_until = self._preset_confirm
            if self._presets[i] is not None and not (c_slot == i and time.monotonic() < c_until):
                self._preset_confirm = (i, time.monotonic() + 3)   # (replace: press again)
                return
            try:
                preset_write(self.state, preset_path(i), i + 1, f"{W}x{H}", config.pi_serial())
            except OSError as e:
                log.error("Could not save preset %d: %s", i + 1, e)
                self.show_toast("Could not save\n(is the SD card full?)", 2.5)
                return
            log.info("Preset %d saved (%s)", i + 1, preset_summary(self.state))
            self.presets_refresh()
            self.show_toast(f"Saved as preset {i + 1}\n(on the SD card)", 2)
        elif name in ("press", "left"):
            self.presets_open = False

    def peek_overlay(self):
        """(v55) changing Rotation or Mirror in the MENU: the picture itself
        for a moment, the choice in a small card at the bottom (as the C
        program; v54: the white balance)"""
        key, label, _, show = self.menu_rows[self.menu_idx]
        note = label[:1].lower() + label[1:]
        t = f"\u2039  {show(self.menu_value(key))}  \u203a"

        def draw(d):
            fl, fv = brand_font(12), brand_font(16, True)
            w = int(max(d.textlength(t, font=fv), d.textlength(note, font=fl))) + 40
            h = 50
            x0, y0 = (W - w) // 2, H - h - 12
            d.rectangle((x0, y0, x0 + w - 1, y0 + h - 1), fill=ZI_CREAM + (255,))
            d.rectangle((x0, y0, x0 + 5, y0 + h - 1), fill=ZI_RED + (255,))
            d.text((x0 + 6 + (w - 6) / 2, y0 + 15), note, font=fl, fill=ZI_GREY + (255,), anchor="mm")
            d.text((x0 + 6 + (w - 6) / 2, y0 + 34), t, font=fv, fill=ZI_INK + (255,), anchor="mm")
        return self.cached(("peek", t, note, W, H), draw)

    def menu_overlay(self):
        if self.presets_open:
            return self.presets_overlay()
        if getattr(self, "peek_until", 0) > time.monotonic():
            return self.peek_overlay()
        rows_all = self.menu_rows
        self.menu_top = min(max(self.menu_top, self.menu_idx - MENU_ROWS + 1), self.menu_idx)
        vals = tuple(self.menu_value(k) for k, *_ in rows_all)
        key = ("menu", self.menu_idx, self.menu_top, vals, len(rows_all))
        action = rows_all[self.menu_idx][0] in ACTIONS

        def draw(d):                                # (v52: Zero Ichi's design - see ui_card)
            ui_card(d, "menu", self.menu_top > 0)
            y = 49
            rows = rows_all[self.menu_top:self.menu_top + MENU_ROWS]
            f = brand_font(14)
            for i, (k, label, _, fmt) in enumerate(rows, start=self.menu_top):
                sel = i == self.menu_idx
                val = fmt(self.menu_value(k))
                shown = val
                if sel and k not in ACTIONS:
                    shown = f"\u2039 {val} \u203a"
                    if d.textlength(label + shown, font=f) > W - 52:
                        shown = val                         # too long: no arrows
                ui_row(d, y, label, shown, sel)
                y += 23
            ui_hint(d, "PRESS: do it" if action else "PRESS: close", self.menu_top + MENU_ROWS < len(rows_all))

        return self.cached(key, draw)

    def toast_overlay(self, text):
        def draw(d):                                # (v52: a cream card with a red edge)
            f = brand_font(15)
            lines = []                              # (long lines wrap to fit the screen)
            for ln in text.split("\n"):
                cur = ""
                for word in ln.split():
                    test = f"{cur} {word}".strip()
                    if cur and d.textlength(test, font=f) > W - 52:
                        lines.append(cur)
                        cur = word
                    else:
                        cur = test
                if cur:
                    lines.append(cur)
            if not lines:
                return
            w = min(W - 8, max(int(d.textlength(ln, font=f)) for ln in lines) + 40)
            h = 24 * len(lines) + 16
            x0, y0 = (W - w) // 2, (H - h) // 2
            d.rectangle((x0, y0, x0 + w - 1, y0 + h - 1), fill=ZI_CREAM + (255,))
            d.rectangle((x0, y0, x0 + 5, y0 + h - 1), fill=ZI_RED + (255,))
            for i, ln in enumerate(lines):
                d.text((x0 + 6 + (w - 6) / 2, y0 + 20 + 24 * i), ln, font=f, fill=ZI_INK + (255,), anchor="mm")
        return self.cached(("toast", text, W, H), draw)

    def fit_screen(self, a, into=None):
        """Fit any picture (240x240, 320x240, 160x120, 240x320...) onto the screen
        (240x240 or 320x240), with black bars where needed. A half-size picture
        (Smooth live view) is doubled with plain copies; other sizes use
        Pillow's nearest-neighbour resize (C code) - much faster than numpy.
        `into`: a screen-sized array to draw into (the screen's send buffer)."""
        pil = isinstance(a, Image.Image)
        w, h = a.size if pil else (a.shape[1], a.shape[0])
        x0, y0, nw, nh = self.view = self._fit_geom(w, h)
        if into is not None and into.shape == (H, W, 3) and (a.mode == "RGB" if pil else
                                                             a.ndim == 3 and a.shape[2] == 3):
            area = into[y0:y0 + nh, x0:x0 + nw]
            if pil:                                     # Pillow picture: Pillow enlarges
                area[...] = np.asarray(a if (nw, nh) == (w, h)
                                       else a.resize((nw, nh), Image.NEAREST))
            elif (nw, nh) == (2 * w, 2 * h):            # Smooth live view: double it
                wide = np.repeat(a, 2, axis=1)          # each pixel twice ...
                area[0::2] = wide                       # ... and each row twice
                area[1::2] = wide
            elif (nw, nh) == (w, h):
                np.copyto(area, a)
            else:
                area[...] = np.asarray(Image.fromarray(np.ascontiguousarray(a)).resize(
                    (nw, nh), Image.NEAREST))
            if y0:                                      # black bars (the buffer is reused)
                into[:y0] = 0
                into[y0 + nh:] = 0
            if x0:
                into[:, :x0] = 0
                into[:, x0 + nw:] = 0
            return into
        if (h, w) == (H, W):
            return np.array(a) if pil else a
        if (nw, nh) != (w, h):
            a = (a if pil else Image.fromarray(np.ascontiguousarray(a))).resize((nw, nh), Image.NEAREST)
            pil = True
        if pil:
            a = np.array(a)                             # the one conversion (writeable)
        if (nw, nh) == (W, H):
            return a
        out = self._canvas.get((h, w))                  # black bars stay black
        if out is None:
            out = self._canvas[(h, w)] = np.zeros((H, W, 3), np.uint8)
        out[y0:y0 + nh, x0:x0 + nw] = a
        return out

    def _fit_geom(self, w, h):
        """(x, y, width, height) of a w x h picture fitted onto the screen."""
        c = self._fit_cache.get(("fit", h, w, W, H))
        if c is None:
            sc = min(W / w, H / h)
            nw, nh = max(1, round(w * sc)), max(1, round(h * sc))
            c = self._fit_cache[("fit", h, w, W, H)] = ((W - nw) // 2, (H - nh) // 2, nw, nh)
        return c

    def fit565(self, a, buf):
        """Put a small 16-bit picture (h x w screen pixels) onto the screen's
        16-bit send buffer, enlarged to fit: the live view's half-size picture
        is doubled with two copies per row (each pixel written as a pair, in
        one go); other sizes use Pillow's quick nearest-neighbour resize."""
        h, w = a.shape
        x0, y0, nw, nh = self.view = self._fit_geom(w, h)
        scr = buf.view(np.uint16)[..., 0]
        area = scr[y0:y0 + nh, x0:x0 + nw]
        if nw == 2 * w and nh == 2 * h:
            wide = np.empty((h, w), np.uint32)
            np.multiply(a, _PAIR, out=wide)             # each pixel twice (as a pair)
            wide = wide.view(np.uint16)
            area[0::2] = wide                           # ... and each row twice
            area[1::2] = wide
        elif nw == w and nh == h:
            area[...] = a
        else:
            area[...] = np.asarray(Image.fromarray(np.ascontiguousarray(a)).resize(
                (nw, nh), Image.NEAREST))
        if y0:                                          # black bars (the buffer is reused)
            scr[:y0] = 0
            scr[y0 + nh:] = 0
        if x0:
            scr[:, :x0] = 0
            scr[:, x0 + nw:] = 0
        return buf

    def label_overlay(self, text, color=(120, 255, 120)):
        return self.cached(("label", text, color), lambda d: self.text(
            d, (W // 2, H - 16), text, self.f18, fill=color))

    def is_square_filter(self):
        return filters.FILTERS[self.state["filter"]][0] in SQUARE_FILTERS

    def sizing_state(self):
        """The settings the shared size calculations need (camera.py)."""
        return dict(self.state, filter_name=filters.FILTERS[self.state["filter"]][0])

    def _geo(self, what, make):
        """Sizes and shapes worked out from the settings, kept until a setting
        they depend on changes - they're needed several times per frame, and
        working them out again each time is a noticeable cost on a Pi Zero."""
        st = self.state
        key = (what, st["filter"], st["size"], st["cam_rotate"], st["mirror"], st["clip_size"],
               st["mode"], W, H)
        v = self._geo_cache.get(key)
        if v is None:
            if len(self._geo_cache) > 64:
                self._geo_cache.clear()
            v = self._geo_cache[key] = make()
        return v

    def aspect(self):
        """Width/height of pictures: Holga is always square (6x6 film)."""
        from camera import picture_aspect
        return self._geo("aspect", lambda: picture_aspect(self.sizing_state(), W, H))

    def target_size(self, photo=False, kind=None):
        """Pixel size of a photo ("photo"), a GIF/video frame ("clip") or the
        live view ("view": the picture's shape, fitted to the screen)."""
        from camera import target_size_for
        kind = kind or ("photo" if photo else "view")
        return self._geo(kind, lambda: target_size_for(self.sizing_state(), W, H, kind))

    def preview_size_for_rotation(self):
        """Camera stream size: screen-sized for the live view; the clip size only
        while a GIF / video is being recorded."""
        from camera import stream_size_for
        if self.rec and self.rec.get("hw"):     # hardware encoder records the stream as is
            tw, th = self.target_size(kind="clip")
            return (th, tw) if self.flips()[2] else (tw, th)
        return stream_size_for(self.sizing_state(), W, H, recording=bool(self.rec))

    def flips(self):
        from camera import flips_for
        return self._geo("flips", lambda: flips_for(self.state["cam_rotate"], self.state["mirror"]))

    def frame_from(self, full, photo=False, cam=None, kind="view"):
        """Turn a camera image the way the menu says, cut out the largest centred
        area with the picture's shape, and scale it to the target size.
        Flips are done by the camera sensor when it can (free); only a 90-degree
        turn is done here - for photos with Pillow's fast C code."""
        cam = cam or self.cam
        h, v, turn = self.flips()
        a = full
        if not cam.hw_flip:                             # sensor couldn't flip
            if h:
                a = a[:, ::-1]
            if v:
                a = a[::-1]
        aspect = self.aspect()
        a = crop_to_aspect(a, 1 / aspect if turn else aspect)   # crop before turning
        tw, th = self.target_size(kind=kind)
        if turn:
            a = np.rot90(a, -1)
        if a.shape[1] == tw and a.shape[0] == th:
            return a
        # Shrink with Pillow's C code (much faster on a Pi Zero than numpy) and
        # keep the result as a Pillow image: the filters and the screen step
        # take it as it is, so the frame isn't converted back and forth.
        im = Image.fromarray(np.ascontiguousarray(a))
        f = min(im.width // tw, im.height // th)
        if f >= 2 and im.width == tw * f and im.height == th * f:
            return im.reduce(f)                         # exact: average f x f blocks
        return im.resize((tw, th), Image.NEAREST)

    def crop_planes(self, planes):
        """The camera's (Y, U, V) planes (U, V at half size) flipped and cut
        to the picture's shape - still sideways when the picture is turned 90
        degrees (turn that after the look: fewer pixels to move). Views only."""
        h, v, turn = self.flips()
        Y, U, V = planes
        if not self.cam.hw_flip:                            # sensor couldn't flip
            if h:
                Y, U, V = Y[:, ::-1], U[:, ::-1], V[:, ::-1]
            if v:
                Y, U, V = Y[::-1], U[::-1], V[::-1]
        a = self.aspect()
        a = 1 / a if turn else a
        ph, pw = Y.shape
        if pw / ph > a + 0.01:                              # too wide: trim the sides
            nw = int(round(ph * a)) & ~1
            x = ((pw - nw) // 2) & ~1
            return Y[:, x:x + nw], U[:, x // 2:(x + nw) // 2], V[:, x // 2:(x + nw) // 2]
        if pw / ph < a - 0.01:                              # too tall: trim top/bottom
            nh = int(round(pw / a)) & ~1
            y = ((ph - nh) // 2) & ~1
            return Y[y:y + nh], U[y // 2:(y + nh) // 2], V[y // 2:(y + nh) // 2]
        return Y, U, V

    def pixel_view(self, planes, palette=None):
        """16-Bit live view: (palette numbers on the view's pixel grid, its
        palette). The camera's planes are cut to the picture's shape first and
        the numbers turned afterwards, so the grid always fits the screen
        exactly (also with the camera turned 90 degrees)."""
        h, v, turn = self.flips()
        if isinstance(planes, tuple):
            src = self.crop_planes(planes)
        else:                                               # (no small stream: R,G,B)
            a = np.asarray(planes)
            if not self.cam.hw_flip:
                if h:
                    a = a[:, ::-1]
                if v:
                    a = a[::-1]
            asp = self.aspect()
            src = np.ascontiguousarray(crop_to_aspect(a, 1 / asp if turn else asp))
        idx, pp, _ = filters._pixel_index(src, palette)
        if turn:
            idx = np.rot90(idx, -1)
        return idx, pp

    @property
    def cam_missing(self):
        return getattr(self.cam, "missing", False)

    def start_camera_retry(self):
        h, v, _ = self.flips()
        self.cam_retry = CameraLoader(swap_rb=self.args.swap_rb, hflip=h, vflip=v,
                                      preview_size=self.preview_size_for_rotation())

    def pump_camera_retry(self):
        """Main loop: switch to the real camera as soon as it has been found."""
        r = self.cam_retry
        if r is None or r.is_alive():
            return
        self.cam_retry = None
        if r.cam is not None:
            self.cam = r.cam
            self.apply_camera_orientation()
            self.cam.set_ev(self.state["ev"])
            self._ov_cache.pop("nocam", None)
            log.info("Camera found after all (%s)", ", ".join(
                f"{k} {v:.1f}s" for k, v in getattr(r.cam, "timing", {}).items()))
            self.show_toast("Camera ready", 1.5)
        else:
            log.info("Still no camera (%s) - trying again", r.error)
            time.sleep(1)
            self.start_camera_retry()

    def nocam_overlay(self):
        def draw(d):
            self.text(d, (W // 2, H // 2 - 14), "No camera found", self.f22, fill=(255, 120, 120))
            self.text(d, (W // 2, H // 2 + 14), "check the ribbon cable", self.f14)
            self.text(d, (W // 2, H // 2 + 34), "still trying...", self.f14, fill=(190, 190, 210))
        return self.cached(("nocam", W, H), draw)

    def apply_camera_orientation_for_recording(self):
        self.rec = {"kind": "starting"}                  # so the size is the clip size
        self.apply_camera_orientation()

    def apply_camera_orientation(self):
        """Set up the camera for the current mode (see camera.stream_plan). The
        same setup is used while recording, so starting a recording doesn't
        change anything the auto-exposure has to readjust to."""
        h, v, turn = self.flips()
        from camera import stream_plan, stream_size_for
        main, lores, photo = stream_plan(self.sizing_state(), W, H)
        video = self.state["mode"] == "VIDEO" and not (self.hw_video_failed or self.video_rgb)
        self.cam.reconfigure(preview_size=main, hflip=h, vflip=v, lores=lores, main_yuv=photo,
                             fallback_size=stream_size_for(self.sizing_state(), W, H),
                             video_yuv=video)

    # -- media files -----------------------------------------------------
    def list_media(self):
        try:
            files = [p for p in self.photo_dir.iterdir()
                     if p.name[:4] in ("IMG_", "GIF_", "VID_")
                     and p.suffix.lower() in (".jpg", ".gif", ".mov")]
        except OSError:
            return []
        return sorted(files, key=_media_order)

    def _first_free_number(self):
        """The next file number: after the highest one on the card, and never
        one used before (so copying to a computer never overwrites anything)."""
        top = 0
        try:
            for p in self.photo_dir.iterdir():
                n = _media_number(p.name)
                if n is not None:
                    top = max(top, n)
        except OSError:
            pass
        return max(top + 1, int(self.state.get("next_number") or 1))

    def new_media_path(self, prefix, ext):
        """Short names, numbered like a camera's: IMG_0001.jpg, GIF_0002.gif,
        VID_0003.mov - one count for all three, so they stay in shooting order."""
        if self._next_num is None:
            self._next_num = self._first_free_number()
        n = self._next_num
        while True:
            path = self.photo_dir / f"{prefix}_{n:04d}{ext}"
            taken = path in self._reserved or any(
                (self.photo_dir / f"{p}_{n:04d}{e}").exists()
                for p, e in (("IMG", ".jpg"), ("GIF", ".gif"), ("VID", ".mov"), ("VID", ".part")))
            if not taken:
                break
            n += 1
        self._next_num = n + 1
        self.state["next_number"] = n + 1             # remembered with the settings
        self.mark_dirty()
        self._reserved.add(path)
        return path

    def drain_events(self):
        try:
            while True:
                self.events.get_nowait()
        except queue.Empty:
            pass

    # -- photo -----------------------------------------------------------
    def photo_settings(self):
        """Everything make_photo needs, frozen at the moment of the shot."""
        h, v, turn = self.flips()
        return {"hflip": h, "vflip": v, "turn": turn, "hw_flip": self.cam.hw_flip,
                "aspect": self.aspect(), "size": self.target_size(photo=True),
                "filter": filters.FILTERS[self.state["filter"]][1],
                "filter_name": filters.FILTERS[self.state["filter"]][0]}

    def take_photo(self):
        from camera import PHOTO_SIZE, PHOTO_SIZE_SIDEWAYS
        if self.saver.full():                         # two photos already waiting
            self.message("Saving...", "one moment")
            while self.saver.full():
                time.sleep(0.05)
        snap = self.photo_settings()
        path = self.new_media_path("IMG", ".jpg")
        self.lcd.show(np.full((H, W, 3), 255, np.uint8))   # "flash"
        try:
            if getattr(self.cam, "photo_stream", False):
                arr = self.cam.capture_still()          # a copy of the current frame
            else:
                arr = self.cam.capture(PHOTO_SIZE_SIDEWAYS if snap["turn"] else PHOTO_SIZE)
        except Exception as e:
            log.exception("Capture failed")
            self._reserved.discard(path)
            self.message("Capture failed", str(e)[:28], bg=(90, 10, 10))
            time.sleep(2)
            self.drain_events()
            return
        self.saver.submit(self._save_photo, arr, snap, path)
        self.show_toast("Click!", 0.6)
        self.drain_events()

    def _save_photo(self, arr, snap, path):           # runs in the Saver thread
        t = time.monotonic()
        res = make_photo(arr, snap)
        del arr
        t_pic = time.monotonic()
        if res[0] == "yuv":
            planes = res[1:]
            h, w = planes[0].shape
            jm = JpegMaker(w, h, JPEG_QUALITY, bits=3.0)
            data = jm.yuv(*planes)
        else:
            img = res[1]
            a = np.asarray(img.convert("RGB") if isinstance(img, Image.Image) and img.mode != "RGB"
                           else img)
            jm = JpegMaker(a.shape[1], a.shape[0], JPEG_QUALITY, bits=3.0)
            data = jm.rgb(a)
        jm.close()
        from movie import with_exif
        data = with_exif(data, snap["filter_name"])    # (who made it, the look - movie.py)
        t_jpg = time.monotonic()
        with open(path, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())                       # safe even if power is pulled
        self._free_at = 0                              # free space changed
        log.info("Saved %s in %.1f s (picture %.1f s, JPEG %.1f s %s %s, write %.1f s)",
                 path.name, time.monotonic() - t, t_pic - t, t_jpg - t_pic, res[0], jm.how(),
                 time.monotonic() - t_jpg)
        return ("Photo saved", path, ("photo", snap.get("filter_name", "Normal"),
                                      jm.w * jm.h, len(data)))

    def pump_saver(self):
        """Pick up results from background saves (main thread)."""
        try:
            while True:
                status, value = self.developer.results.get_nowait()
                if status == "ok":
                    self._reserved.discard(value[1])
                    if len(value) > 2:
                        self.learn_space(*value[2])
                    self.count += 1
                    self.show_toast("Video developed" if "no filter" not in value[0]
                                    else "Video saved (no filter)", 1.5)
                else:
                    self.show_toast("Developing failed!", 2)
        except queue.Empty:
            pass
        try:
            while True:
                status, value = self.saver.results.get_nowait()
                if status == "ok":
                    text, path = value[:2]
                    self._reserved.discard(path)
                    if len(value) > 2:
                        self.learn_space(*value[2])
                    self.count += 1
                else:
                    self._reserved = {p for p in self._reserved if p.exists()}
                    self.show_toast("Save failed!", 2)
        except queue.Empty:
            pass

    def wait_for_saves(self, why="Saving..."):
        if self.saver.pending:
            self.message(why, "one moment")
            self.saver.wait()
            self.pump_saver()

    # -- GIF & video recording -------------------------------------------
    def trigger(self):
        if self.cam_missing:
            self.show_toast("No camera!", 1.5)
            return
        if self.card_full():
            self.show_toast("Card full!", 2)
            return
        mode = self.state["mode"]
        if mode == "PHOTO":
            self.take_photo()
        elif mode == "GIF":
            self.rec = {"kind": "gif", "frames": [], "times": [],
                        "target": self.state["gif_frames"],
                        "filter": filters.FILTERS[self.state["filter"]][0]}
            self.apply_camera_orientation()              # stream -> clip size
        else:
            if self.instant_video():
                self.start_live_video()
                return
            if not self.hw_video_failed and self.start_hw_video():
                return
            self.apply_camera_orientation_for_recording()
            self.rec = {"kind": "video", "writer": None, "start": time.monotonic(),
                        "path": self.new_media_path("VID", ".mov"),
                        "filter": filters.FILTERS[self.state["filter"]][0]}

    def instant_video(self, name=None):
        """Filtered videos are recorded "instantly" at every clip size (from
        v47): the look is applied while recording, at the live view's size,
        enlarged smoothly to the clip size, and the video is saved straight
        away - nothing to develop. Normal videos are recorded by the hardware
        encoder (no look), saved at once too."""
        name = name or filters.FILTERS[self.state["filter"]][0]
        return name != "Normal"

    def instant_size(self, name=None):
        """Size of instant videos: the clip size (320x240, square: 240x240) -
        for every look (v53: the Game Boy looks too, as on the screen; until
        v52 they came in whole steps of their 128-pixel grid, only 3 sizes)."""
        return self.target_size(kind="clip")

    def start_live_video(self):
        """Instant video: the live view's own frames, with the look already on
        them, are compressed as they come (see LiveEncoder)."""
        name = filters.FILTERS[self.state["filter"]][0]
        w, h = self.instant_size(name)
        path = self.new_media_path("VID", ".mov")
        raw = path.with_suffix(".part")
        writer = MovWriter(str(raw), w, h, quality=VIDEO_QUALITY, look=name)
        enc = LiveEncoder(writer, (w, h), "yuv420")
        self.rec = {"kind": "video", "live": True, "writer": writer, "encoder": enc,
                    "path": path, "raw": raw, "start": time.monotonic(), "filter": name,
                    "shown": 0, "cpu": CpuMeter(), "size": (w, h)}

    def finish_live_video(self, r):
        cpu = r["cpu"].report()
        enc, w, raw, path = r["encoder"], r["writer"], r["raw"], r["path"]
        if enc.jobs.qsize() > 3:
            self.message("Saving...", "one moment")    # (the last few frames)
        try:
            enc.finish()
            w.close()
            n, dur = len(w), w.duration()
            if n < 2 or enc.error:
                raw.unlink(missing_ok=True)
                self._reserved.discard(path)
                self.show_toast("Video failed!" if enc.error else "Video too short", 2)
                return
            os.replace(raw, path)
            os.sync()
            self._reserved.discard(path)
            self.count += 1
            missed = sum(max(0, round((b - a) * VIDEO_FPS) - 1)
                         for a, b in zip(w.times, w.times[1:]))
            log.info("Recorded %s instantly: %d frames in %.1f s = %.1f frames/s (%s, %dx%d), "
                     "%d camera frames skipped, %d dropped by the writer; compressing %.0f ms "
                     "per frame (%s)", path.name, n, dur, (n - 1) / dur if dur else 0,
                     r["filter"], w.w, w.h, missed, enc.dropped,
                     1000 * enc.spent / max(1, enc.frames), enc.jm.how())
            log.info("CPU while recording: %s; Pi: %s", cpu, pi_health())
            self.learn_space("video", r["filter"], w.w * w.h * n, path.stat().st_size)
            self.show_toast(f"Saved {dur:.0f} s video")
        except Exception:
            log.exception("Saving the video failed")
            self.show_toast("Video failed!", 2)
        finally:
            self.drain_events()

    def start_hw_video(self):
        """Record with the hardware encoder (smooth, steady frame rate). Returns
        False if that isn't possible, so the software recorder is used instead."""
        self.rec = {"kind": "starting", "hw": True}
        self.apply_camera_orientation()              # stream = the clip size
        w, h = self.cam.preview_size
        turn = self.flips()[2]
        path = self.new_media_path("VID", ".mov")
        raw = path.with_suffix(".part")
        writer = MovWriter(str(raw), w, h, quality=VIDEO_QUALITY, rotation=90 if turn else 0,
                           look=filters.FILTERS[self.state["filter"]][0])
        try:
            try:
                self.cam.start_video(writer, fps=VIDEO_FPS)
            except Exception as e:
                if not getattr(self.cam, "video_yuv", False):
                    raise
                # the encoder didn't take the YUV stream: use RGB from now on
                log.warning("Hardware video from the YUV stream failed (%s) - using RGB", e)
                self.video_rgb = True
                self.apply_camera_orientation()
                self.cam.start_video(writer, fps=VIDEO_FPS)
        except Exception as e:
            log.warning("Hardware video recording not available (%s) - using software", e)
            self.hw_video_failed = True
            writer.close()
            raw.unlink(missing_ok=True)
            self._reserved.discard(path)
            self.rec = None
            return False
        self.rec = {"kind": "video", "hw": True, "writer": writer, "path": path, "raw": raw,
                    "start": time.monotonic(), "filter": filters.FILTERS[self.state["filter"]][0],
                    "cpu": CpuMeter()}
        # The recording comes first: the live view (this thread) steps back so
        # picamera2's threads, which hand the frames to the encoder, never wait.
        set_thread_priority(5)
        return True

    def rec_frame(self, out, now):
        """Add a frame to the recording. Returns True when the recording ended."""
        r = self.rec
        if r["kind"] == "gif":
            r["frames"].append(np.array(out))       # unfiltered; filtered when saved
            r["times"].append(now)
            if len(r["frames"]) >= r["target"]:
                self.finish_gif()
                return True
            return False
        try:
            if r["writer"] is None:
                r["writer"] = MovWriter(str(r["path"]), *filters.size_of(out),
                                        quality=VIDEO_QUALITY, look=r.get("filter"))
            r["writer"].add_frame(np.ascontiguousarray(filters.as_array(out)), now)
        except Exception:
            log.exception("Video write failed")
            self.finish_video()
            return True
        if self.video_over(now):
            self.finish_video()
            return True
        return False

    def video_over(self, now):
        """The recording is as long as set in MENU > Video max, or its file is
        nearly as big as a file on the card's FAT drive can be (4 GB)."""
        r = self.rec
        m = self.state["video_max"]
        if m and now - r["start"] >= m:
            return True
        if now - r.get("_size_at", 0.0) > 2.0:          # (the file size: every 2 s)
            r["_size_at"] = now
            for k in ("raw", "path"):
                try:
                    if r.get(k) and os.path.getsize(r[k]) > MAX_VIDEO_BYTES:
                        self.show_toast("Video stopped:\nfile full (3.5 GB)", 3)
                        return True
                except OSError:
                    pass
        return False

    def rec_pace(self):
        """While the hardware encoder records: the shortest time one live-view
        frame may take. The video comes first - it must never lose a frame - so
        this starts at about 12 frames a second (or where the last recording
        ended) and adapts every second: if a video frame was missed, the live
        view slows down; while every frame arrives, it speeds up again (up to
        the camera's 15 frames a second)."""
        r = self.rec
        if not r or not r.get("hw") or r.get("writer") is None:
            return 0.15
        now = time.monotonic()
        if "pace" not in r:                              # (start where the last one ended)
            r.update(pace=self._pace_start, pace_at=now, pace_seen=0, dropped=0, pace_sum=0.0,
                     pace_n=0)
        if now - r["pace_at"] >= 1.0:
            times = r["writer"].times
            n = len(times)
            new = times[max(0, r["pace_seen"] - 1):n]
            gaps = [b - a for a, b in zip(new, new[1:])]
            missed = sum(1 for g in gaps if g > 1.5 / VIDEO_FPS)
            if missed and n > 4:                         # (the first frames may be uneven)
                r["pace"] = min(0.30, r["pace"] + 0.05)
                r["dropped"] += missed
            elif len(gaps) >= VIDEO_FPS * 0.6:           # frames are coming in fine
                r["pace"] = max(0.0, r["pace"] - 0.02)
            r["pace_seen"], r["pace_at"] = n, now
            r["pace_sum"] += r["pace"]
            r["pace_n"] += 1
        return r["pace"]

    @staticmethod
    def log_recording(r, frames, seconds, what):
        """Log the real frame rate and where the time went (for tuning)."""
        p = r.get("perf")
        if not p or not p[0]:
            return
        n = p[0]
        fps = (frames - 1) / seconds if seconds > 0 and frames > 1 else 0
        log.info("%s: %d frames in %.1f s = %.1f frames/s; per frame: camera %.0f ms, "
                 "filter %.0f ms, screen %.0f ms, %s %.0f ms", what, frames, seconds, fps,
                 1000 * p[1] / n, 1000 * p[2] / n, 1000 * p[3] / n,
                 "compress" if what.startswith("Video") else "store", 1000 * p[4] / n)

    def stop_recording(self):
        if self.rec and self.rec["kind"] == "gif":
            self.finish_gif()
        elif self.rec:
            self.finish_video()

    def finish_gif(self):
        r, self.rec = self.rec, None
        self.apply_camera_orientation()                  # stream back to screen size
        frames, times = r["frames"], r["times"]
        if len(frames) < 2:
            self.show_toast("GIF cancelled")
            return
        gaps = [b - a for a, b in zip(times, times[1:])]
        ms = max(60, round(1000 * sum(gaps) / len(gaps) / 10) * 10)
        self.log_recording(r, len(frames), times[-1] - times[0],
                           f"GIF {frames[0].shape[1]}x{frames[0].shape[0]}")
        path = self.new_media_path("GIF", ".gif")
        self.saver.submit(self._save_gif, frames, ms, path, r.get("filter", "Normal"),
                          lambda: bool(self.rec) and self.rec.get("kind") == "video")
        self.show_toast("Saving GIF...", 1.0)
        self.drain_events()

    @staticmethod
    def _save_gif(frames, ms, path, name="Normal", hold=None):   # Saver thread
        t = time.monotonic()
        fn = dict(filters.FILTERS).get(name, filters.normal)
        tone = filters.TONE_FILTERS.get(name)
        ctone = filters.COLOR_TONES.get(name)

        def wait():                     # a video is being recorded: it goes first
            while hold is not None and hold():
                time.sleep(0.2)
        if ctone is not None:               # 16-Bit: already a palette picture too
            imgs = []
            for f in frames:
                wait()
                imgs.append(ctone(np.ascontiguousarray(f)).gif_frame())
        elif tone is not None:
            # black & white looks: each frame is already a palette picture -
            # no colour reduction needed (that is most of the work otherwise)
            imgs = []
            for f in frames:
                wait()
                imgs.append(tone(filters._luma(f)).gif_frame())
        else:
            done = []
            for f in frames:
                wait()
                done.append(filters.for_clip(name, fn, f))
            frames = done
            # One shared palette for all frames: faster, and no colour flicker.
            sample = np.concatenate([filters.as_array(f)[::4, ::4] for f in frames], axis=0)
            palette = Image.fromarray(np.ascontiguousarray(sample)).quantize(
                colors=128, method=Image.Quantize.FASTOCTREE)
            imgs = []
            for f in frames:
                wait()
                imgs.append(filters._pil(f).quantize(palette=palette, dither=Image.Dither.NONE))
        with open(path, "wb") as f:
            from movie import gif_comment
            imgs[0].save(f, format="GIF", save_all=True, append_images=imgs[1:],
                         duration=ms, loop=0, comment=gif_comment(name).encode())
            f.flush()
            os.fsync(f.fileno())
        os.sync()
        log.info("Saved %s (%d frames, %d ms) in %.1f s", path.name, len(frames), ms,
                 time.monotonic() - t)
        w, h = imgs[0].size
        try:
            nbytes = path.stat().st_size
        except OSError:
            nbytes = 0
        return ("GIF saved", path, ("gif", name, w * h * len(imgs), nbytes))

    def finish_hw_video(self, r):
        set_thread_priority(0)                           # live view back to normal
        seconds = max(0.1, time.monotonic() - r["start"])
        cpu = r["cpu"].report() if r.get("cpu") else None
        try:
            self.cam.stop_video()
        except Exception:
            log.exception("Stopping the video encoder failed")
        self.apply_camera_orientation()                  # stream back to screen size
        w, raw, path = r["writer"], r["raw"], r["path"]
        try:
            w.close()
            os.sync()
            n, dur = len(w), w.duration()
            if n < 2:
                raw.unlink(missing_ok=True)
                self._reserved.discard(path)
                self.show_toast("Video too short")
                return
            missed = sum(max(0, round((b - a) * VIDEO_FPS) - 1)
                         for a, b in zip(w.times, w.times[1:]))
            pace = r["pace_sum"] / r["pace_n"] if r.get("pace_n") else r.get("pace", 0.15)
            if "pace" in r:                              # the next recording starts from here
                self._pace_start = min(0.15, r["pace"] + 0.02)
            log.info("Recorded %s: %d frames in %.1f s = %.1f frames/s (hardware encoder, %s), "
                     "%d frames missed; screen showed %.1f frames/s (paced at %.0f ms)",
                     path.name, n, dur, (n - 1) / dur if dur else 0,
                     r["filter"], missed, r.get("shown", 0) / seconds, 1000 * pace)
            if cpu:
                log.info("CPU while recording: %s; Pi: %s", cpu, pi_health())
            if r["filter"] == "Normal":                  # nothing to develop: ready now
                self.learn_space("video", "Normal", w.w * w.h * n, raw.stat().st_size)
                os.replace(raw, path)
                os.sync()
                self._reserved.discard(path)
                self.count += 1
                self.show_toast(f"Saved {dur:.0f} s video")
            else:
                note = raw.with_name(raw.name + ".json")
                with open(note, "w") as f:
                    json.dump({"final": path.name, "filter": r["filter"]}, f)
                    f.flush()
                    os.fsync(f.fileno())
                os.sync()
                self.developer.submit(raw)
                self.enter_developing()
        except Exception:
            log.exception("Saving the video failed")
            self.show_toast("Video failed!", 2)
        finally:
            self.drain_events()

    def finish_video(self):
        r, self.rec = self.rec, None
        if r.get("live"):
            self.finish_live_video(r)
            return
        if r.get("hw"):
            self.finish_hw_video(r)
            return
        self.apply_camera_orientation()                  # stream back to screen size
        w = r["writer"]
        self._reserved.discard(r["path"])
        try:
            if w is not None:
                w.close()                                # writes the index + fsync
                os.sync()
            if w is None or len(w) < 2:
                if r["path"].exists():
                    r["path"].unlink()
                self.show_toast("Video too short")
            else:
                log.info("Saved %s (%d frames, %.1f s)", r["path"].name, len(w), w.duration())
                self.log_recording(r, len(w), w.duration(), f"Video {w.w}x{w.h}")
                self.count += 1
                self.show_toast(f"Saved {w.duration():.0f} s video")
        except Exception as e:
            log.exception("Video save failed")
            self.show_toast("Video failed!", 2)
        self.drain_events()

    # -- camera / menu screens -------------------------------------------
    def tick_camera(self):
        now = time.monotonic()
        cpu0 = time.thread_time()
        name, flt = filters.FILTERS[self.state["filter"]]
        kind = self.rec.get("kind") if self.rec else None
        recording = kind in ("gif", "video")
        hw = recording and self.rec.get("hw", False)
        live = recording and self.rec.get("live", False)      # instant video
        sw_video = kind == "video" and not hw and not live
        ctone = filters.COLOR_TONES.get(name)
        tone = filters.TONE_FILTERS.get(name)
        # The picture goes straight into the screen's next send buffer. A 16-bit
        # screen gets it in its own format, made while the picture is still
        # small (a quarter of the pixels), then enlarged.
        buf = self.lcd.frame_buffer() if hasattr(self.lcd, "frame_buffer") else None
        fast = buf is not None and buf.shape[-1] == 2
        rec_item = small = out = raw = planes = None
        # Normal, Cross on a 16-bit screen: camera YUV -> screen pixels
        yuv_look = fast and not live and name in filters.YUV_LOOKS
        film = name == "Zero Ichi"                  # (v52: colour film on the camera's Y, U, V)
        hcol = name == "Holga Color"                # (v55: on the camera's Y, U, V too)
        if kind == "gif" or sw_video:   # need the full frame too (same camera frame)
            full, big = self.cam.frame_pair()
        elif (ctone is not None or yuv_look or film or hcol) and hasattr(self.cam, "frame_yuv"):
            planes = self.cam.frame_yuv()   # straight from the camera's YUV
        else:                           # black & white looks: brightness plane only
            full = big = self.cam.frame(gray=tone is not None)
        t_cam = time.monotonic()
        if sw_video:            # software video: filter the full clip frame now
            out = filters.for_clip(name, flt, self.frame_from(big, kind="clip"))
            if fast:
                small = rgb_to_565(np.asarray(out))
        elif planes is not None and film and isinstance(planes, tuple):    # Zero Ichi
            Y, U, V = self.crop_planes(planes)
            if self.flips()[2]:                     # (turned: before the grain and vignette)
                Y, U, V = (np.rot90(p, -1) for p in (Y, U, V))
            Y, U, V = (np.ascontiguousarray(p) for p in (Y, U, V))
            if live:
                yz, uz, vz = filters.zeroichi_yuv(Y, U, V)
                rec_item = ("yuv", (yz, uz, vz))
            else:
                yz = filters.zeroichi_luma(Y)
            if fast:
                small = filters.yuv_to_565(yz, U, V, filters.yuv565_table(name))
            else:
                if not live:
                    uz, vz = filters.zeroichi_colour(Y, U, V)
                uv = [Image.fromarray(p).resize((yz.shape[1], yz.shape[0]), Image.NEAREST) for p in (uz, vz)]
                out = Image.merge("YCbCr", [Image.fromarray(yz)] + uv).convert("RGB")
        elif planes is not None and hcol and isinstance(planes, tuple):    # Holga Color
            Y, U, V = self.crop_planes(planes)
            if self.flips()[2]:
                Y, U, V = (np.rot90(p, -1) for p in (Y, U, V))
            yh, uh, vh = filters.holga_color_yuv(*(np.ascontiguousarray(p) for p in (Y, U, V)))
            if live:
                rec_item = ("yuv", (yh, uh, vh))
            if fast:
                small = filters.yuv_to_565(yh, uh, vh, filters.yuv565_table("Normal"))
            else:
                uv = [Image.fromarray(p).resize((yh.shape[1], yh.shape[0]), Image.NEAREST) for p in (uh, vh)]
                out = Image.merge("YCbCr", [Image.fromarray(yh)] + uv).convert("RGB")
        elif planes is not None and ctone is None:      # Normal, Cross, Holga Color (no small stream)
            if isinstance(planes, tuple):
                small = filters.yuv_to_565(*self.crop_planes(planes), filters.yuv565_table(name))
                if self.flips()[2]:
                    small = np.rot90(small, -1)
            else:                           # (no small stream: R,G,B)
                small = rgb_to_565(np.asarray(flt(self.frame_from(planes, kind="view"))))
        elif planes is not None:            # 16-Bit: palette numbers on the view's grid
            idx, pp = self.pixel_view(planes, filters.pixel_palette_of(name))
            if fast:
                small = np.take(pp.pal.p565, idx)         # one look-up per pixel
            else:
                out = np.take(pp.rgb32, idx).view(np.uint8).reshape(idx.shape + (4,))[..., :3]
            if live:
                rec_item = ("tone", filters.pixel_tone_of(idx, pp))
        elif tone is not None:              # black & white looks: tones + a palette
            if kind == "gif":   # GIF: keep the unfiltered frame; filtered when saved
                raw = self.frame_from(big, kind="clip")
            tn = tone(self.frame_from(full, kind="view"))
            if fast:
                t = tn.t if tn.t.size == tn.size else tn.t.resize(tn.size, Image.NEAREST)
                small = np.take((tn.pal or filters._PAL_GRAY).p565, np.asarray(t))
            else:
                out = tn.rgb()
            if live:
                rec_item = ("tone", tn)
        else:                   # live view: shrink to screen size first, then filter
            if kind == "gif":
                raw = self.frame_from(big, kind="clip")
            out = flt(self.frame_from(full, kind="view"))
            if live:
                rec_item = ("rgb", out)
            if fast:
                small = rgb_to_565(np.asarray(out))
        t_filter = time.monotonic()

        clip = out if sw_video else raw
        if fast:
            out = self.fit565(small, buf)
        else:
            shown = self.fit_screen(out, into=buf)
            if (sw_video or live) and shown is out:     # don't draw the HUD into the recording
                shown = out.copy()
            out = shown
        if self.screen == "menu":
            out = self.compose(out, self.menu_overlay())
        else:
            out = self.compose(out, self.hud())
            sp = self.space_overlay()
            if sp is not None:
                out = self.compose(out, sp)
            if self.cam_missing:
                out = self.compose(out, self.nocam_overlay())
            if recording:
                out = self.compose(out, self.rec_overlay())
            if recording:                                   # progress bar
                if self.rec["kind"] == "gif":
                    p = len(self.rec["frames"]) / self.rec["target"]
                else:
                    m = self.state["video_max"]
                    p = (now - self.rec["start"]) / m if m else 0.0
                n = int(W * min(1.0, p))
                if out.shape[-1] == 2:                      # (16-bit screen frame)
                    out.view(np.uint16)[H - 4:, :n, 0] = REC_RED_565
                else:
                    out[H - 4:, :n] = REC_RED
        if self.toast:
            if now < self.toast[1]:
                out = self.compose(out, self.toast_overlay(self.toast[0]))
            else:
                self.toast = None
        if buf is not None and out is buf:
            self.lcd.show_buffer_async(out)   # sent while the next frame is prepared
        else:
            self.lcd.show_async(out)
        if not recording:                 # measure the live view (logged every 30 s)
            lv = self._lv_perf
            end = time.monotonic()
            if lv[7] and now - lv[7] > 0.5:   # a pause (recording, menu, test): left out
                lv[4] += now - lv[7]
            lv[7] = end
            lv[0] += 1
            lv[1] += t_cam - now
            lv[2] += t_filter - t_cam
            lv[3] += end - t_filter
            lv[5] += end - now               # time spent in the live view only
            lv[6] += time.thread_time() - cpu0   # ... of which really working (not waiting)
            if now - lv[4] >= 30 and lv[0]:
                n = lv[0]
                log.info("Live view: %.1f frames/s; per frame: camera %.0f ms, filter %.0f ms, "
                         "screen %.0f ms, own work %.0f ms [%s, %dx%d, %s, %d-bit screen]; "
                         "CPU: %s; Pi: %s", n / max(now - lv[4], 0.01), 1000 * lv[1] / n,
                         1000 * lv[2] / n, 1000 * lv[3] / n, 1000 * lv[6] / n, name,
                         *self.target_size(kind="view"), self.state["mode"],
                         getattr(self.lcd, "bits", 16), self._cpu.report(), pi_health())
                self._lv_perf = [0, 0.0, 0.0, 0.0, now, 0.0, 0.0, lv[7]]
        if live:                          # instant video: this frame goes in as it is
            self.rec["encoder"].put(rec_item, now - self.rec["start"])
            self.rec["shown"] = self.rec.get("shown", 0) + 1
            if self.video_over(now):
                self.finish_video()
        elif hw:                          # the hardware encoder does the recording
            self.rec["shown"] = self.rec.get("shown", 0) + 1
            if self.video_over(now):
                self.finish_video()
        elif recording:
            t_show = time.monotonic()
            perf = self.rec.setdefault("perf", [0, 0.0, 0.0, 0.0, 0.0])
            perf[0] += 1
            perf[1] += t_cam - now
            perf[2] += t_filter - t_cam
            perf[3] += t_show - t_filter
            self.rec_frame(clip, now)     # compress/store after it's on screen
            perf[4] += time.monotonic() - t_show

    def on_camera(self, name):
        st = self.state
        if name == "key1":
            if self.rec:
                self.stop_recording()
            else:
                self.trigger()
        elif name in ("left", "right"):                    # works while recording too
            step = 1 if name == "right" else -1
            before = self.aspect()
            st["filter"] = (st["filter"] + step) % len(filters.FILTERS)
            if self.aspect() != before and not self.rec:   # e.g. Holga is square
                self.apply_camera_orientation()
            self.mark_dirty()
        elif self.rec:
            return
        elif name in ("up", "down"):
            step = EV_STEP if name == "up" else -EV_STEP
            st["ev"] = max(EV_MIN, min(EV_MAX, st["ev"] + step))
            self.cam.set_ev(st["ev"])
            self.mark_dirty()
        elif name == "press":
            self.open_menu()
        elif name == "key2":
            self.enter_gallery()
        elif name == "key3":
            st["mode"] = MODES[(MODES.index(st["mode"]) + 1) % len(MODES)]
            self.apply_setting("mode")              # GIF/VIDEO use the clip size
            self.mark_dirty()

    def on_menu(self, name):
        if self.presets_open:
            self.on_presets(name)
            return
        rows = self.menu_rows
        peeking = getattr(self, "peek_until", 0) > time.monotonic()
        self.peek_until = 0
        if peeking and name in ("up", "down", "press"):
            return                                      # (just back to the MENU)
        if name in ("up", "down"):
            step = 1 if name == "down" else -1
            self.menu_idx = (self.menu_idx + step) % len(rows)
        elif name in ("left", "right"):
            key = rows[self.menu_idx][0]
            if key in ACTIONS:                          # (nothing to choose)
                return
            choices = next(c for k, _, c, _ in rows if k == key)
            i = choices.index(self.menu_value(key)) + (1 if name == "right" else -1)
            if key == "screen":
                self.switch_screen(choices[i % len(choices)])
            else:
                self.state[key] = choices[i % len(choices)]
                self.apply_setting(key)
                self.mark_dirty()
                if key in ("cam_rotate", "mirror", "size"):     # (the picture itself for a moment)
                    self.peek_until = time.monotonic() + 3.0
        elif name == "press" and rows[self.menu_idx][0] in ACTIONS:
            key = rows[self.menu_idx][0]
            getattr(self, key)(self.slots[key])          # e.g. about()
        elif name in ("press", "key1", "key2"):
            self.screen = "camera"
            self.save_state()

    # -- "developing film" screen ------------------------------------------
    def enter_developing(self):
        """While a filtered video develops, pause the camera so the developer
        gets the whole processor (and show its progress). Any key goes back to
        the camera; developing then carries on in the background."""
        self.screen = "developing"
        self.developer.preview = None
        self._dev_shown = 0.0
        if hasattr(self.cam, "pause"):
            self.cam.pause()

    def leave_developing(self, done=False):
        if hasattr(self.cam, "resume"):
            self.cam.resume()
        self.screen = "camera"
        if not done:
            self.show_toast("Developing in background", 1.5)

    def tick_developing(self):
        if not self.developer.pending:
            self.pump_saver()
            self.leave_developing(done=True)
            return
        now = time.monotonic()
        if now - self._dev_shown < 0.5:               # redraw twice a second
            return
        self._dev_shown = now
        p = self.developer.progress or 0.0
        img = Image.new("RGB", (W, H), (16, 16, 22))
        prev = self.developer.preview
        if prev is not None:
            thumb = Image.fromarray(prev)
            thumb.thumbnail((int(W * 0.72), int(H * 0.52)))
            img.paste(thumb, ((W - thumb.width) // 2, 40))
        d = ImageDraw.Draw(img)
        self.text(d, (W // 2, 20), "Developing film...", self.f18, fill=(255, 220, 80))
        y = H - 58
        d.rectangle((20, y, W - 20, y + 14), outline=(200, 200, 210))
        d.rectangle((22, y + 2, 22 + int((W - 44) * p), y + 12), fill=(255, 220, 80))
        self.text(d, (W // 2, y - 12 if prev is None else y - 10), f"{int(p * 100)}%", self.f14)
        if self.developer.pending > 1:
            self.text(d, (W // 2, y + 28), f"{self.developer.pending} videos to go", self.f14,
                      fill=(190, 190, 210))
        self.text(d, (W // 2, H - 12), "any key: back to the camera", self.f14,
                  fill=(150, 150, 170))
        self.lcd.show_pil(img)

    def on_developing(self, name):
        self.leave_developing()

    # -- gallery ---------------------------------------------------------
    def enter_gallery(self):
        self.wait_for_saves()
        self.media = self.list_media()
        self.gidx = len(self.media) - 1
        self.screen = "gallery"
        self.load_item()

    def _gif_decode(self, item):
        """Decode the next GIF frame (frames are kept, so later loops are free)."""
        im = item["gif"]
        try:
            im.seek(len(item["frames"]))
        except EOFError:
            item["gif_done"] = True
            im.close()
            return False
        item["frames"].append(self.screen_frame(im))
        item["durs"].append(max(0.04, im.info.get("duration", 100) / 1000))
        if len(item["frames"]) >= 60:
            item["gif_done"] = True
            im.close()
        return True

    def load_item(self):
        old = self.gal
        if old is not None:
            if old.get("reader") is not None:
                old["reader"].close()
            if old.get("gif") is not None and not old.get("gif_done"):
                old["gif"].close()
        self.gal = None
        if not self.media:
            self.message("No photos yet", "KEY2: back to camera")
            return
        path = self.media[self.gidx]
        kind = {"IMG_": "photo", "GIF_": "gif", "VID_": "video"}[path.name[:4]]
        item = {"path": path, "kind": kind, "frames": [], "durs": [], "i": 0,
                "next": 0.0, "reader": None, "error": False}
        try:
            if kind == "photo":
                with Image.open(path) as im:
                    im.draft("RGB", (W, H))           # fast JPEG downscale on decode
                    item["frames"].append(np.asarray(
                        ImageOps.pad(im.convert("RGB"), (W, H), Image.BILINEAR, color=(0, 0, 0))))
            elif kind == "gif":
                # Only the first frame now; the rest are decoded as they play.
                item["gif"] = Image.open(path)
                item["gif_done"] = False
                self._gif_decode(item)
            else:
                item["reader"] = MovReader(str(path))
                rd = item["reader"]
                item["frames"].append(self.fit_screen(self.video_frame(rd, 0)).copy())
        except Exception as e:
            log.warning("Can't open %s: %s", path.name, e)
            item["error"] = True
            item["reader"] = None
            item["gif_done"] = True
            item["frames"] = [np.full((H, W, 3), (60, 0, 0), np.uint8)]
        item["ov"] = self.gallery_overlay(item)
        self.gal = item
        self.show_item_frame()

    def gallery_overlay(self, item):
        path = item["path"]
        stamp = path.stem                       # (v53: its name only - no date)
        tag = {"photo": "", "gif": "GIF", "video": "VIDEO"}[item["kind"]]
        if item["reader"] is not None:
            s = int(round(item["reader"].duration()))
            tag += f" {s // 60}:{s % 60:02d}"
        pos = f"<  {self.gidx + 1}/{len(self.media)}  >"

        def draw(d):
            self.text(d, (W // 2, 14), stamp, self.f14)
            if tag:
                self.text(d, (6, 36), tag, self.f14, fill=MODE_COLORS["GIF" if
                          item["kind"] == "gif" else "VIDEO"], anchor="lm")
            if item["error"]:
                self.text(d, (W // 2, H // 2), "Can't open file", self.f18)
            elif item["kind"] == "video":
                self.text(d, (W // 2, H // 2 - 8), "\u25b6", self.f30)
                self.text(d, (W // 2, H // 2 + 22), "PRESS TO PLAY", self.f14)
            self.text(d, (W // 2, H - 14), pos, self.f18)

        return self.overlay(draw)

    def screen_frame(self, img):
        """A Pillow image -> a full-screen frame (fast nearest scaling + bars)."""
        return self.fit_screen(np.asarray(img.convert("RGB") if img.mode != "RGB" else img)).copy()

    def draft_size(self, w, h):
        """Size to ask the JPEG decoder for: it can decode at 1/2, 1/4 or 1/8 size
        almost for free; accept anything at least ~35% of the screen size
        (quick playback; enlarged like the live view)."""
        sc = min(W / w, H / h) * 0.35
        return (max(1, int(w * sc)), max(1, int(h * sc)))

    def video_frame(self, rd, i):
        """Frame i of a video, decoded at about screen size, turned upright."""
        w, h = rd.size
        turned = rd.rotation in (90, 270)
        dw, dh = self.draft_size(h, w) if turned else self.draft_size(w, h)
        a = rd.frame_array(i, (dh, dw) if turned else (dw, dh))
        if rd.rotation == 90:
            a = np.rot90(a, -1)
        elif rd.rotation == 270:
            a = np.rot90(a, 1)
        elif rd.rotation == 180:
            a = a[::-1, ::-1]
        return a

    def show_item_frame(self):
        g = self.gal
        self.lcd.show(self.compose(g["frames"][g["i"]].copy(), g["ov"]))

    def tick_gallery(self):
        g = self.gal
        if not g or g["kind"] != "gif" or g["error"]:
            return
        now = time.monotonic()
        if now < g["next"]:
            return
        nxt = g["i"] + 1
        if nxt >= len(g["frames"]):
            if not g["gif_done"] and self._gif_decode(g):
                pass                                    # decoded the next frame
            else:
                nxt = 0                                 # loop
        if nxt < len(g["frames"]) and len(g["frames"]) > 1:
            g["i"] = nxt
            g["next"] = now + g["durs"][g["i"]]
            self.show_item_frame()

    def play_video(self):
        """Play a video on the screen in real time: frames are decoded at reduced
        size, scaled quickly, and sent to the screen in the background while the
        next one is decoded. If the Pi falls behind, frames are skipped rather
        than slowing the video down."""
        rd = self.gal["reader"]
        self.drain_events()
        shown, t_start = 0, time.monotonic()
        t0 = time.monotonic()
        due = 0.0                                       # when frame i should show
        for i in range(len(rd)):
            if not self.events.empty():
                break                                   # any key stops playback
            due_next = due + rd.durations[i]
            if time.monotonic() - t0 > due_next:        # already too late for this one
                due = due_next
                continue
            a = self.video_frame(rd, i)
            buf = self.lcd.frame_buffer() if hasattr(self.lcd, "frame_buffer") else None
            if buf is not None and buf.shape[-1] == 2:      # 16-bit: convert while small
                frame = self.fit565(rgb_to_565(a), buf)
            else:
                frame, buf = self.fit_screen(a).copy(), None
            wait = t0 + due - time.monotonic()
            if wait > 0:
                time.sleep(wait)
            if buf is not None:
                self.lcd.show_buffer_async(frame)
            else:
                self.lcd.show_async(frame)
            shown += 1
            due = due_next
        self.lcd.wait()
        took = time.monotonic() - t_start
        self.last_input = time.monotonic()          # (the screen saver counts from now)
        log.info("Played %s: showed %d of %d frames (%.1f frames/s)",
                 self.gal["path"].name, shown, len(rd), shown / max(took, 0.1))
        self.drain_events()
        self.show_item_frame()

    def on_gallery(self, name):
        if name in ("key1", "key2"):
            self.screen, self.gal = "camera", None
        elif not self.media:
            return
        elif name in ("left", "right"):
            step = 1 if name == "right" else -1
            self.gidx = (self.gidx + step) % len(self.media)
            self.load_item()
        elif name == "up":
            self.gidx = 0
            self.load_item()
        elif name == "down":
            self.gidx = len(self.media) - 1
            self.load_item()
        elif name == "press":
            if self.gal and self.gal["reader"] is not None:
                self.play_video()
        elif name == "key3":
            self.screen = "confirm"
            what = {"photo": "photo", "gif": "GIF", "video": "video"}[self.gal["kind"]]
            self.message(f"Delete {what}?", "KEY3 = yes   other = no", bg=(90, 10, 10))

    def on_confirm(self, name):
        if name == "key3":
            try:
                self.media[self.gidx].unlink()
                os.sync()
                self.media.pop(self.gidx)
                self.count = max(0, self.count - 1)
                self.gidx = max(0, min(self.gidx, len(self.media) - 1))
                self.message("Deleted")
                time.sleep(0.5)
            except OSError as e:
                self.message("Delete failed", str(e)[:28])
                time.sleep(1.5)
        self.screen = "gallery"
        self.load_item()

    # -- power -----------------------------------------------------------
    def power_off(self):
        if self.rec:
            self.stop_recording()
        self.save_state()
        self.wait_for_saves("Finishing saving...")
        try:
            import bootopt
            if bootopt.RUNNING.is_set():
                self.message("Finishing setup...", "one moment")
                bootopt.wait_until_done()
        except Exception:
            pass
        if self.args.allow_shutdown:
            if self.developer.pending:
                log.info("Video developing will continue at the next start")
            self.message("Shutting down...", "unplug when the green LED is off")
            self.cleanup(keep_screen=True)          # message stays up until power-off
            self.remember_clock()
            os.sync()
            if fast_start_running():
                # Zero Ichi's own start-up does the rest: closes the card, switches off
                self.save_screen_message(brand_image(
                    ["Switched off", "", "Unplug when the", "green light is off."]))
                log.info("Power off")
                logging.shutdown()
                os.sync()
                os._exit(EXIT_POWEROFF)
            sudo = [] if os.geteuid() == 0 else ["sudo", "-n"]
            # Fast power-off: skips stopping every service one by one, but still
            # stops all programs and unmounts the SD card safely before power-off.
            for cmd in (["systemctl", "poweroff", "--force"], ["systemctl", "poweroff"]):
                try:
                    if subprocess.run(sudo + cmd, check=False, timeout=20).returncode == 0:
                        break
                except Exception as e:
                    log.warning("%s failed: %s", " ".join(cmd), e)
            # Never return from here: if Zero Ichi exited, the system would just
            # start it again (that's what looked like a "restart").
            while True:
                time.sleep(1)
        else:
            self.message("Bye!")
            time.sleep(0.8)
        self.running = False

    @staticmethod
    def remember_clock(quick=False):
        """The Pi has no clock battery. The time last seen is kept by Zero Ichi's
        own files on the SD card (their "modified" date) - touched here at
        power-off and every few minutes, in case the power is pulled - and the
        next start carries on from there (clock_from_card)."""
        try:                               # (the card's own file: its date is the time)
            if STATE_FILE.exists():
                os.utime(STATE_FILE, None)
        except OSError:
            pass
        if quick:
            return
        import shutil
        if shutil.which("fake-hwclock"):                   # older systems
            try:
                subprocess.run(["fake-hwclock", "save"], timeout=10,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except Exception:
                pass

    # -- main loop -------------------------------------------------------
    def handle(self, name, kind):
        self.last_input = time.monotonic()
        if name == "key3" and kind == "hold":
            if self.state.get("key3") == "Standby":
                if self.asleep:
                    self.wake_up()
                elif self.screen in ("camera", "menu", "gallery", "about"):
                    self.screen = "camera"
                    self.standby()
                return
            self.power_off()
            return
        if self.asleep:                           # a key wakes the screen - nothing more
            self.wake_up()
            return
        getattr(self, f"on_{self.screen}")(name)

    def run(self):
        # Everything loaded at start-up (numpy, Pillow, picamera2...: a lot of
        # objects) is never scanned by Python's garbage collector again - a
        # full scan takes long enough on a Pi Zero to make the live view jerk.
        import gc
        gc.freeze()
        self.maybe_offer_space()
        try:
            while self.running:
                try:
                    while self.running:
                        event = self.events.get_nowait()
                        try:
                            self.handle(*event)
                        except Exception:              # log it, don't crash/restart
                            log.exception("Error handling %s", event)
                except queue.Empty:
                    pass
                if not self.running:
                    break
                self.pump_saver()
                self.pump_camera_retry()
                if self.asleep:                       # screen off / the screen saver
                    if self.asleep == 2:
                        self.saver_tick()
                        time.sleep(min(0.1, max(0.01, self._ss_next - time.monotonic())))
                    else:
                        time.sleep(0.1)
                    continue
                if self.screen in ("camera", "menu"):
                    t_tick = time.monotonic()
                    self.tick_camera()
                    if self.rec and self.rec.get("hw"):
                        # recording: the live view runs as fast as it can without
                        # the video losing a frame (see rec_pace)
                        time.sleep(max(0.0, self.rec_pace() - (time.monotonic() - t_tick)))
                    if self.rec and self.rec.get("kind") == "video" and self.card_full():
                        self.finish_video()                # stop before the card is full
                        self.show_toast("Card full!", 2)
                    elif self.developer.pending and not self.rec:
                        # a video is developing in the background: show ~4 frames a
                        # second so the developer gets most of the processor
                        time.sleep(max(0.0, 0.25 - (time.monotonic() - t_tick)))
                    elif self.saver.pending and not self.rec:
                        # a photo or GIF is being saved: a slightly calmer live view
                        # (~7 frames a second) lets the save finish much sooner
                        time.sleep(max(0.0, 0.14 - (time.monotonic() - t_tick)))
                elif self.screen == "gallery":
                    self.tick_gallery()
                    time.sleep(0.01)
                elif self.screen == "developing":
                    self.tick_developing()
                    time.sleep(0.1)
                elif self.screen == "space":
                    self.tick_space()
                    time.sleep(0.05)
                else:
                    time.sleep(0.03)
                if (self._dirty_at and not self.rec
                        and time.monotonic() - self._dirty_at > 0.5):
                    self.save_state()
                if time.monotonic() - self._clock_at > 300 and not self.rec:
                    self._clock_at = time.monotonic()
                    self.remember_clock(quick=True)
                self.maybe_sleep()
        finally:
            self.cleanup()

    def cleanup(self, keep_screen=False):
        if self._cleaned:
            return
        self._cleaned = True
        try:
            if self.rec:
                self.stop_recording()
            if self._dirty_at:
                self.save_state()
            self.saver.wait(timeout=60)
        except Exception:
            log.exception("Could not finish recording")
        screen = self.lcd.release if keep_screen else self.lcd.close
        for fn in (self.cam.close, self.controls.close, screen):
            try:
                fn()
            except Exception:
                pass



def _boot_report(ready_after, marks):
    """zeroichi/boot-time.txt: where the Pi spends its start-up time."""
    time.sleep(3)
    set_thread_priority(19)
    try:
        import subprocess
        steps = "  ".join(f"{name} {t:.1f}s" for name, t in (marks or []))
        fast = fast_start_running()
        out = [f"Zero Ichi v{config.VERSION} ready {ready_after:.1f} s after power-on "
               f"({'fast start' if fast else 'normal start-up'})\n  [{steps}]\n"]
        cmds = ["systemd-analyze critical-chain zeroichi.service --no-pager",
                "systemd-analyze blame --no-pager | head -15", "systemd-analyze"]
        if fast:                       # no systemd: the fast start's own timings
            cmds = []
            try:
                lines = Path("/run/zeroichi/fast-start.txt").read_text().splitlines()
                steps = sorted((float(ln.split(" ", 1)[0]), ln.split(" ", 1)[1].rsplit(" @", 1)[0])
                               for ln in lines if " " in ln)     # (" @N": idle time - C report)
                out.append("Fast start (seconds after the kernel started):\n" + "\n".join(
                    f"  {t:6.2f} s  {text}" for t, text in steps) + "\n")
            except (OSError, ValueError):
                pass
            try:
                errs = Path("/run/zeroichi/errors.txt").read_text().strip()
                if errs:
                    out.append("Messages:\n" + errs[-3000:] + "\n")
            except OSError:
                pass
            with open(APP_DIR / "boot-time.txt", "w") as f:
                f.write("\n".join(out))
                f.flush()
                os.fsync(f.fileno())
        for c in cmds:
            # lowest priority: systemd-analyze is heavy on a Pi Zero and must never
            # slow down the camera (it used to, right after start-up)
            r = subprocess.run(f"nice -n 19 ionice -c 3 {c}", shell=True, capture_output=True,
                               text=True, timeout=180)
            out.append(f"$ {c}\n{(r.stdout + r.stderr).strip()}\n")
            with open(APP_DIR / "boot-time.txt", "w") as f:   # write as it goes
                f.write("\n".join(out))
                f.flush()
                os.fsync(f.fileno())
    except Exception as e:
        log.info("No boot-time report: %s", e)


def main(settings=None, lcd=None, started=None, progress=None, camera_loader=None,
         marks=None):
    settings = settings or config.settings()
    p = argparse.ArgumentParser(description="Zero Ichi for Pi Zero + Waveshare 1.3in LCD HAT")
    p.add_argument("--photo-dir")
    p.add_argument("--rotate", type=int, choices=(0, 180),
                   help="initial screen flip (the menu setting overrides it)")
    p.add_argument("--swap-rb", action="store_true", help="swap red/blue if colours look wrong")
    p.add_argument("--spi-speed", type=int)
    p.add_argument("--allow-shutdown", action="store_true",
                   help="holding KEY3 powers off the Pi instead of just quitting")
    p.add_argument("--no-camera", action="store_true",
                   help="use a moving test pattern instead of the camera")
    p.set_defaults(**settings)          # settings.txt, overridden by the command line
    args = p.parse_args()
    if args.rotate not in (0, 180):
        args.rotate = 0
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))

    log.info("Settings: %s", vars(args))
    cam_app = ZeroIchi(args, lcd=lcd, camera_loader=camera_loader, progress=progress)
    if started:
        cam_app.ready_after = started()
        steps = "  ".join(f"{name} {t:.1f}s" for name, t in (marks or []))
        log.info("Ready %.1f s after power-on  [%s]", cam_app.ready_after, steps)
    try:                                # (v53: the fast start loads its other drivers now)
        Path("/run/zeroichi/camera-ready").touch()
    except OSError:
        pass

    # One-time boot speed-up (or undo it). Done now, before the live preview
    # starts, so it has the CPU to itself and is never cut off at power-off.
    try:
        import bootopt
        bootopt.radios_off()                         # (0.1.0: never any Wi-Fi or Bluetooth)
        if getattr(args, "optimize_boot", True) and bootopt.needed():
            cam_app.message("One-time setup", "making start-up faster...")
            t = time.monotonic()
            bootopt.optimize()
            log.info("Boot optimizer took %.0f s", time.monotonic() - t)
        elif not getattr(args, "optimize_boot", True) and bootopt.revert_needed():
            cam_app.message("Undoing fast boot", "one moment...")
            bootopt.revert()
        if getattr(args, "optimize_boot", True):
            bootopt.sync_fast_start(getattr(args, "fast_start", True),
                                    os.environ.get("ZEROICHI_RUN_DIR") or APP_DIR)
    except Exception:
        log.exception("Boot optimizer failed")

    if started:
        threading.Thread(target=_boot_report, args=(cam_app.ready_after, marks),
                         daemon=True, name="boot-report").start()
    try:
        cam_app.run()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    main()
