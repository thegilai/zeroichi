"""
Toy-camera filters.

Every filter takes an HxWx3 uint8 RGB numpy array and returns one of the same
shape. The same code runs on the live preview and on the full-size photo, so
what you see is what you get. Built from lookup tables and small images where
possible, to stay fast on a single-core Pi Zero.
"""
import random

import numpy as np
from PIL import Image, ImageFilter

_rng = np.random.default_rng()        # big one-off noise pictures
_pr = random.Random()                 # per-frame dice: Python's own is ~10x lighter


def _pick(n):
    """A random whole number 0 .. n-1 (cheap enough to call every frame)."""
    return int(_pr.random() * n)
_X = np.arange(256, dtype=np.float32)


# --- helpers -------------------------------------------------------------
def _lut(values):
    return np.clip(values, 0, 255).astype(np.uint8)


def _sigmoid(k):
    """S-shaped contrast curve, 0..255 in, 0..255 out (float)."""
    x = _X / 255.0
    y = 1.0 / (1.0 + np.exp(-k * (x - 0.5)))
    lo = 1.0 / (1.0 + np.exp(k * 0.5))
    hi = 1.0 / (1.0 + np.exp(-k * 0.5))
    return (y - lo) / (hi - lo) * 255.0


def _palette_lut(stops):
    """Build a 256x3 colour map by interpolating between RGB stops."""
    stops = np.array(stops, dtype=np.float32)
    pos = np.linspace(0, 255, len(stops))
    return np.stack([np.interp(_X, pos, stops[:, c]) for c in range(3)],
                    axis=1).astype(np.uint8)


def _gray(img):
    i = img.astype(np.uint16)
    return ((i[..., 0] * 77 + i[..., 1] * 150 + i[..., 2] * 29) >> 8).astype(np.uint8)


def _apply3(img, lr, lg, lb):
    out = np.empty(img.shape, np.uint8)
    out[..., 0] = lr[img[..., 0]]
    out[..., 1] = lg[img[..., 1]]
    out[..., 2] = lb[img[..., 2]]
    return out


def _from_gray(g, lr, lg, lb):
    out = np.empty(g.shape + (3,), np.uint8)
    out[..., 0] = lr[g]
    out[..., 1] = lg[g]
    out[..., 2] = lb[g]
    return out


def _saturate(img, k):
    i = img.astype(np.int32)
    g = (i[..., 0] * 77 + i[..., 1] * 150 + i[..., 2] * 29) >> 8
    out = g[..., None] + (((i - g[..., None]) * int(k * 256)) >> 8)
    return np.clip(out, 0, 255).astype(np.uint8)


def _scale(img):
    return max(1, img.shape[1] // 240)


def _soft(img, eff_width):
    """Blur by shrinking to ~eff_width pixels wide and scaling back up, so the
    softness looks the same on the preview and the full-size photo."""
    h, w = img.shape[:2]
    if w <= eff_width:
        return img
    sw, sh = eff_width, max(1, round(h * eff_width / w))
    im = Image.fromarray(np.ascontiguousarray(img))
    return np.asarray(im.resize((sw, sh), Image.BILINEAR).resize((w, h), Image.BILINEAR))


def _shrink(img, grid_w):
    """Scale down so the picture is grid_w pixels across (keeps the shape)."""
    h, w = img.shape[:2]
    gw = grid_w if w >= h else max(8, round(grid_w * w / h))
    gh = max(8, round(gw * h / w))
    im = Image.fromarray(np.ascontiguousarray(img))
    return np.asarray(im.resize((gw, gh), Image.BILINEAR))


_up_cache = {}


def _enlarge(small, h, w):
    """Blow a small picture up to h x w with big square pixels."""
    sh, sw = small.shape[:2]
    if h % sh == 0 and w % sw == 0:
        return np.repeat(np.repeat(small, h // sh, axis=0), w // sw, axis=1)
    key = (sh, sw, h, w)
    idx = _up_cache.get(key)
    if idx is None:
        if len(_up_cache) > 8:
            _up_cache.clear()
        idx = _up_cache[key] = ((np.arange(h) * sh // h)[:, None],
                                (np.arange(w) * sw // w)[None, :])
    return small[idx[0], idx[1]]


_radial_cache = {}


def _radial(h, w):
    """Distance from the centre, 0 (middle) .. 1 (corners), float32 h x w.
    Computed at 1/4 size and enlarged - it's a smooth gradient, so that's
    invisible, and much faster for full-size photos on a Pi Zero."""
    key = (h, w)
    d = _radial_cache.get(key)
    if d is None:
        f = 4 if min(h, w) >= 64 else 1
        sh, sw = -(-h // f), -(-w // f)
        y = ((np.arange(sh, dtype=np.float32) + 0.5) * f - h / 2) / (h / 2)
        x = ((np.arange(sw, dtype=np.float32) + 0.5) * f - w / 2) / (w / 2)
        d = np.sqrt(y[:, None] ** 2 + x[None, :] ** 2) / np.sqrt(2.0)
        if f > 1:
            d = np.repeat(np.repeat(d, f, axis=0), f, axis=1)[:h, :w]
        if len(_radial_cache) > 4:
            _radial_cache.clear()
        _radial_cache[key] = d
    return d


_vcache = {}


def _vignette(img, strength=0.6):
    h, w = img.shape[:2]
    key = (h, w, strength)
    m = _vcache.get(key)
    if m is None:
        m = ((1.0 - strength * _radial(h, w) ** 2.2) * 256).astype(np.uint16)[..., None]
        if len(_vcache) > 6:
            _vcache.clear()
        _vcache[key] = m
    return ((img.astype(np.uint16) * m) >> 8).astype(np.uint8)


def _vignette_gray(g, strength=0.6):
    """Vignette on a single-channel image (a third of the work of colour)."""
    h, w = g.shape
    key = (h, w, strength)
    m = _vcache.get(key)
    if m is None:
        m = ((1.0 - strength * _radial(h, w) ** 2.2) * 256).astype(np.uint16)[..., None]
        _vcache[key] = m
    return ((g.astype(np.uint16) * m[..., 0]) >> 8).astype(np.uint8)


_gate_cache = {}


def _gate(h, w):
    """Mask (uint8 0/1) of a film gate: rounded corners and a thin black edge.
    Only the four corners need real work."""
    key = (h, w)
    m = _gate_cache.get(key)
    if m is None:
        r = int(min(h, w) * 0.08)
        e = max(1, w // 120)
        m = np.ones((h, w, 1), np.uint8)
        m[:e] = m[-e:] = 0
        m[:, :e] = m[:, -e:] = 0
        if r > 0:
            yy, xx = np.mgrid[0:r, 0:r]
            outside = ((r - yy) ** 2 + (r - xx) ** 2) > r * r   # top-left corner
            c = np.where(outside, 0, 1).astype(np.uint8)[..., None]
            m[e:e + r, e:e + r] &= c
            m[e:e + r, w - e - r:w - e] &= c[:, ::-1]
            m[h - e - r:h - e, e:e + r] &= c[::-1, :]
            m[h - e - r:h - e, w - e - r:w - e] &= c[::-1, ::-1]
        if len(_gate_cache) > 4:
            _gate_cache.clear()
        _gate_cache[key] = m
    return m




# 4x4 Bayer matrix: the ordered "checkerboard" dithering of old consoles
_BAYER4 = (np.array([[0, 8, 2, 10], [12, 4, 14, 6],
                     [3, 11, 1, 9], [15, 7, 13, 5]], np.float32) + 0.5) / 16 - 0.5


def _bayer(h, w):
    return np.tile(_BAYER4, (-(-h // 4), -(-w // 4)))[:h, :w]


# --- lookup tables -------------------------------------------------------
MONO = _lut(_sigmoid(6))
CROSS_R = _lut(_sigmoid(9))
CROSS_G = _lut(_sigmoid(4) * 0.95 + 15)
CROSS_B = _lut(_X * 0.6 + 50)
THERMAL = _palette_lut([(0, 0, 0), (20, 0, 140), (150, 0, 160),
                        (235, 50, 0), (255, 200, 0), (255, 255, 255)])
_EM_BASE = _sigmoid(4.5)
_S8_R, _S8_G, _S8_B = _sigmoid(5.5), _sigmoid(5.5), _sigmoid(5.0)      # Super 8 (Kodachrome-like)
HOLGA = _lut(_sigmoid(8.5) * 1.04 - 6)      # punchy contrast, deep blacks
GB_CONTRAST = _lut(_sigmoid(5))

# Game Boy palettes, darkest to lightest
GB_GREEN = np.array([(15, 56, 15), (48, 98, 48), (139, 172, 15), (155, 188, 15)], np.uint8)
GB_GRAY = np.array([(0, 0, 0), (96, 96, 96), (176, 176, 176), (255, 255, 255)], np.uint8)


# --- Pillow helpers (8-bit C code: much lighter on the Pi Zero's slow memory
#     than numpy's wide temporaries) -------------------------------------------
from PIL import ImageChops, ImageDraw  # noqa: E402

_mask_cache = {}


def _pil(img):
    """numpy array or Pillow image -> Pillow RGB image (no copy if already one)."""
    if isinstance(img, Image.Image):
        return img if img.mode == "RGB" else img.convert("RGB")
    im = Image.fromarray(np.ascontiguousarray(img))
    return im if im.mode == "RGB" else im.convert("RGB")


def as_array(img):
    """Pillow image or numpy array -> numpy array (one copy at most)."""
    return np.asarray(img) if isinstance(img, Image.Image) else img


def size_of(img):
    """(width, height) of a Pillow image or numpy array."""
    return img.size if isinstance(img, Image.Image) else (img.shape[1], img.shape[0])


def _point(im, table):
    """im.point(table), minus Pillow's own per-call work: it rounds every
    entry of the table in Python each time (768 numbers for colour) - a
    noticeable cost per frame on a Pi Zero. Zero Ichi's tables are whole numbers
    already, so they go straight to Pillow's C code."""
    try:
        im.load()
        return im._new(im.im.point(table, None))
    except Exception:
        return im.point(table)


def _shrink_fast(im, gw, gh):
    """Shrink a Pillow image to gw x gh. reduce() (averaging whole blocks) is
    several times faster than a smooth resize, so use it for the big step."""
    f = min(im.width // gw, im.height // gh)
    if f >= 2:
        im = im.reduce(f)
    if im.size != (gw, gh):
        im = im.resize((gw, gh), Image.BILINEAR)
    return im


def _table(*luts):
    """Pillow point() table: one LUT per channel, concatenated."""
    out = []
    for lut in luts:
        out.extend(int(v) for v in lut)
    return out


def _cached(key, make):
    m = _mask_cache.get(key)
    if m is None:
        if len(_mask_cache) > 24:
            _mask_cache.clear()
        m = _mask_cache[key] = make()
    return m


def _smooth_mask(w, h, fn):
    """An 8-bit mask from fn(distance from centre 0..1), worked out at quarter
    size and enlarged smoothly - these masks are soft gradients, so it looks the
    same, and the first photo with a look is ready seconds sooner on a Pi Zero."""
    f = 4 if min(w, h) >= 128 else 1
    sw, sh = -(-w // f), -(-h // f)
    y = (np.arange(sh, dtype=np.float32) + 0.5 - sh / 2) / (sh / 2)
    x = (np.arange(sw, dtype=np.float32) + 0.5 - sw / 2) / (sw / 2)
    d = np.sqrt(y[:, None] ** 2 + x[None, :] ** 2) / np.float32(np.sqrt(2.0))
    g = Image.fromarray(np.clip(fn(d) * 255 + 0.5, 0, 255).astype(np.uint8), "L")
    return g.resize((w, h), Image.BILINEAR) if (sw, sh) != (w, h) else g


def _vig_mask(w, h, strength, mode="L", power=1, gate=False):
    """Vignette (optionally squared, optionally with the film-gate corners) as a
    Pillow image to multiply with - one pass instead of several."""
    def make():
        g = _smooth_mask(w, h, lambda d: (1.0 - strength * d ** 2.2) ** power)
        if gate:                                   # sharp corners: full size, whole numbers
            g = ImageChops.multiply(g, _gate_l(w, h))
        return g if mode == "L" else Image.merge("RGB", (g, g, g))
    return _cached(("vig", w, h, strength, mode, power, gate), make)


def _focus_mask(w, h):
    """255 = sharp (middle) .. 0 = soft (edges): the Holga plastic lens."""
    return _cached(("focus", w, h),
                   lambda: _smooth_mask(w, h, lambda d: np.clip((0.80 - d) / 0.45, 0, 1)))


def _noise(w, h, mode, sigma):
    """A random crop of a pre-made grain image (128 = no change)."""
    def make():
        big = np.clip(128 + _rng.normal(0, sigma, (512, 512)), 0, 255).astype(np.uint8)
        g = Image.fromarray(big, "L")
        return g if mode == "L" else Image.merge("RGB", (g, g, g))
    base = _cached(("noise", mode, sigma), make)
    if w > 512 or h > 512:                       # big photos: coarser grain
        f = -(-max(w, h) // 512)
        tile = _noise(-(-w // f), -(-h // f), mode, sigma)
        return tile.resize((w, h), Image.NEAREST)
    x, y = _pick(513 - w), _pick(513 - h)
    return base.crop((x, y, x + w, y + h))


def _bayer_img(w, h, mode, amount):
    """The 4x4 ordered-dither pattern as an image (128 = no change)."""
    def make():
        b = np.clip(128 + _bayer(h, w) * amount, 0, 255).astype(np.uint8)
        g = Image.fromarray(b, "L")
        return g if mode == "L" else Image.merge("RGB", (g, g, g))
    return _cached(("bayer", w, h, mode, amount), make)


_flicker_tables = {}


def _em_table(gain):
    key = round(gain, 2)
    t = _flicker_tables.get(key)
    if t is None:
        t = _flicker_tables[key] = _table(_lut(_S8_R * 0.90 * key + 20),     # Super 8: warm,
                                          _lut(_S8_G * 0.84 * key + 14),     # deep, golden
                                          _lut(_S8_B * 0.68 * key + 18))     # highlights
    return t


T_MONO = _table(MONO)
T_CROSS = _table(CROSS_R, CROSS_G, CROSS_B)
T_THERMAL = _table(THERMAL[:, 0], THERMAL[:, 1], THERMAL[:, 2])
T_HOLGA = _table(HOLGA)
T_GB_CONTRAST = _table(GB_CONTRAST)


def _gb_table(palette):
    level = [int(min(3, max(0, np.floor(v / 85 + 0.5)))) for v in range(256)]
    return _table([palette[l][0] for l in level], [palette[l][1] for l in level],
                  [palette[l][2] for l in level])


# --- input helpers ---------------------------------------------------------
def _img(img):
    """numpy array or Pillow image -> Pillow image, keeping grey (L) or RGB."""
    if isinstance(img, Image.Image):
        return img if img.mode in ("L", "RGB") else img.convert("RGB")
    a = np.asarray(img)
    if a.ndim == 3 and a.shape[2] == 1:
        a = a[..., 0]
    return Image.fromarray(np.ascontiguousarray(a))


def _luma(img):
    """Brightness only (Pillow "L"). A grey input - e.g. the camera's own
    brightness plane - is used as it is: no colour work at all."""
    im = _img(img)
    return im if im.mode == "L" else im.convert("L")


# --- black & white style filters: a grey "tone" picture + a palette ----------
def _yuv_of(r, g, b):
    """JPEG (full-range BT.601) Y, Cb, Cr of R, G, B values."""
    r, g, b = (np.asarray(x, np.float32) for x in (r, g, b))
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = 128 - 0.168736 * r - 0.331264 * g + 0.5 * b
    v = 128 + 0.5 * r - 0.418688 * g - 0.081312 * b
    return [[int(min(255, max(0, round(float(x))))) for x in c] for c in (y, u, v)]


def rgb565(rgb):
    """R,G,B values (any shape ending in 3) -> 16-bit screen pixels (RGB565),
    stored the way the screen takes them (big-endian) - ready to copy."""
    a = np.asarray(rgb, np.uint16)
    v = ((a[..., 0] & 0xF8) << 8) | ((a[..., 1] & 0xFC) << 3) | (a[..., 2] >> 3)
    return v.astype(">u2", order="C").view(np.uint16)


class Palette:
    """How the 256 tones of a black & white style filter become colours - as an
    RGB table (screen, GIFs), a 16-bit screen table, and Y / U / V tables
    (photos, videos)."""

    def __init__(self, r, g, b):
        r, g, b = ([int(x) for x in c] for c in (r, g, b))
        self.rgb = r + g + b                          # Pillow point() table
        self.p565 = rgb565(np.stack([r, g, b], -1))   # tone -> 16-bit screen pixel
        self.triplets = [c for i in range(256) for c in (r[i], g[i], b[i])]
        self.y, self.u, self.v = _yuv_of(r, g, b)
        self.gray = r == g == b
        # GIFs: one index per distinct colour (Game Boy: just 4); smooth palettes
        # in 64 steps (smooth tones, and files that still compress well)
        few = len(set(zip(r, g, b))) <= 64
        first, idx = {}, []
        for i in range(256):
            j = i if few else min(255, (i // 4) * 4 + 2)
            idx.append(first.setdefault((r[j], g[j], b[j]), j))
        self.gif_index = idx


_const_cache = {}


def _const_plane(w, h, value=128):
    k = (w, h, value)
    if k not in _const_cache:
        if len(_const_cache) > 8:
            _const_cache.clear()
        _const_cache[k] = np.full((h, w), value, np.uint8)
    return _const_cache[k]


class Tone:
    """Result of a black & white style filter: a grey tone picture `t` and its
    palette (None = plain grey). It becomes RGB for the screen, or goes straight
    to YUV planes for photos and videos - no colour conversion either way."""

    def __init__(self, t, pal=None, size=None, indexed=False):
        self.t, self.pal = t, pal
        self.size = tuple(size) if size else t.size
        self.indexed = indexed            # t holds palette numbers: never average them

    sharp = False                         # pixel-art looks: enlarge with sharp pixel edges

    def _sized(self, im, size=None, smooth=False):
        """Enlarge a small (grid) picture to the output size - sharp pixel edges
        (or smoothly, for the photo-like looks of instant videos). Colours are
        always looked up on the small picture first: the same result as
        enlarging first, with far fewer pixels to work on."""
        size = size or self.size
        if im.size == size:
            return im
        return im.resize(size, Image.BILINEAR if smooth and not self.sharp else Image.NEAREST)

    def rgb(self):
        if self.pal is None:
            return self._sized(self.t).convert("RGB")
        if self.t.size != self.size:                  # palette picture: enlarge 1 byte/pixel
            p = Image.frombytes("P", self.t.size, self.t.tobytes())
            p.putpalette(self.pal.triplets)
            return self._sized(p).convert("RGB")
        return _point(self.t.convert("RGB"), self.pal.rgb)

    def gif_frame(self):
        """A palette ("P") picture: GIFs need no colour reduction at all."""
        pal = self.pal or _PAL_GRAY
        g = _point(self.t, pal.gif_index)
        p = Image.frombytes("P", g.size, g.tobytes())
        p.putpalette(pal.triplets)
        return self._sized(p)

    def yuv(self, size=None, smooth=False):
        """(Y, U, V) numpy planes, U and V at half size (4:2:0) - at the Tone's
        size, or at `size` (instant videos)."""
        size = tuple(size) if size else self.size
        w, h = size
        hs = (w // 2, h // 2)
        pal = self.pal
        small = self.t.size != size                   # small grid (Game Boy, 16-Bit)
        y = self.t if pal is None else _point(self.t, pal.y)
        y = self._sized(y, size, smooth)
        if pal is None or pal.gray:
            u = v = _const_plane(*hs)
        else:
            tw, th = self.t.size
            if (smooth and small and not self.indexed and not self.sharp and tw > hs[0] and th > hs[1]
                    and tw % 2 == 0 and th % 2 == 0):
                half = self.t.reduce(2)               # (videos: halved, then enlarged - quick)
            elif small or self.indexed:               # (palette numbers: never averaged)
                half = self.t
            else:
                half = (self.t.reduce(2) if w % 2 == 0 and h % 2 == 0
                        else self.t.resize(hs, Image.BOX))
            u = np.asarray(self._sized(_point(half, pal.u), hs, smooth))
            v = np.asarray(self._sized(_point(half, pal.v), hs, smooth))
        return np.asarray(y), u, v


_PAL_GRAY = None          # (made below, once Palette exists)


def _pal(lut3):
    return Palette(*lut3)


def _compress(lut, a):
    """Squeeze a 0..255 table into a..255-a (room for grain, see _grain_mod)."""
    lut = np.asarray(lut, np.float32)
    return [int(round(a + x * (255 - 2 * a) / 255)) for x in lut]


def _grain_mod(w, h, amp, mode="L"):
    """Film grain for ImageChops.add_modulo: values -amp..amp stored modulo 256.
    The picture is first squeezed into amp..255-amp, so nothing wraps around.
    (Whole-number maths: much lighter than add-with-offset on a Pi Zero.)"""
    def make():
        n = np.clip(np.round(_rng.normal(0, amp / 2.2, (512, 512))), -amp, amp).astype(np.int16)
        g = Image.fromarray((n % 256).astype(np.uint8), "L")
        return g if mode == "L" else Image.merge("RGB", (g, g, g))
    base = _cached(("grainmod", amp, mode), make)
    if w > 512 or h > 512:                            # big photos: coarser grain
        f = -(-max(w, h) // 512)
        return _grain_mod(-(-w // f), -(-h // f), amp, mode).resize((w, h), Image.NEAREST)
    x, y = _pick(513 - w), _pick(513 - h)
    return base.crop((x, y, x + w, y + h))


_PAL_GRAY = Palette(range(256), range(256), range(256))
HOLGA_GRAIN = 26                  # room left in the tone for the grain (the tone tables)
HOLGA_GRAIN_AMP = 14              # how strong the grain is (v45: about half - "less grain")
T_HOLGA_C = _compress(HOLGA, HOLGA_GRAIN)
_EXPAND = [int(min(255, max(0, round((v - HOLGA_GRAIN) * 255 / (255 - 2 * HOLGA_GRAIN)))))
           for v in range(256)]
PAL_HOLGA = Palette(_EXPAND, _EXPAND, _EXPAND)
PAL_THERMAL = Palette(THERMAL[:, 0], THERMAL[:, 1], THERMAL[:, 2])


def _gb_palette(colors):
    level = [int(min(3, max(0, np.floor(v / 85 + 0.5)))) for v in range(256)]
    return Palette([colors[lv][0] for lv in level], [colors[lv][1] for lv in level],
                   [colors[lv][2] for lv in level])


PAL_GB_GREEN = _gb_palette(GB_GREEN)
PAL_GB_GRAY = _gb_palette(GB_GRAY)


def _soften(g, w, h, to_w):
    """Blur by shrinking to `to_w` pixels wide and back (big pictures are first
    averaged down with reduce(), which is several times faster)."""
    th = max(1, round(h * to_w / w))
    small = g
    f = w // (to_w * 2)
    if f >= 2:
        small = small.reduce(f)
    return small.resize((to_w, th), Image.BILINEAR).resize((w, h), Image.BILINEAR)


def _holga_tone(img, size=None):
    """Holga 120 on black & white film: plastic lens that is only sharp in the
    middle, very dark corners, strong contrast, grain and a black film border."""
    g = _luma(img)
    W0, H0 = g.size
    if W0 > 110:
        g = Image.composite(g, _soften(g, W0, H0, 110), _focus_mask(W0, H0))
    g = _point(g, T_HOLGA_C)
    g = ImageChops.add_modulo(g, _grain_mod(W0, H0, HOLGA_GRAIN_AMP))
    g = ImageChops.multiply(g, _vig_mask(W0, H0, 0.55, "L", power=2, gate=True))
    return Tone(g, PAL_HOLGA, size)


# --- Hi-Con B&W: hard street black & white ----------------------------------------
# Inspired by Daido Moriyama's street photographs: pushed film - deep blacks,
# burnt-out whites, dark corners. (0.1.0: sharp - it was softened on purpose
# before, which looked out of focus.)
HICON = _lut(_sigmoid(13) * 1.10 - 18)              # hard: blacks crushed, whites burnt out
T_HICON = _table(HICON)
HICON_VIG = 0.30                                    # (v48: a subtle vignette, no grain)


def _hicon_tone(img, size=None):
    g = _point(_luma(img), T_HICON)
    W0, H0 = g.size
    g = ImageChops.multiply(g, _vig_mask(W0, H0, HICON_VIG, "L"))
    return Tone(g, None, size)


def _mono_tone(img, size=None):
    g = _point(_luma(img), T_MONO)
    return Tone(ImageChops.multiply(g, _vig_mask(g.width, g.height, 0.35)), None, size)


def _thermal_tone(img, size=None):
    return Tone(_luma(img), PAL_THERMAL, size)


# --- Zero Ichi: the camera's own look (v52: colour film) --------------------------
# Colour film with the logo's colours. On the brightness: a film curve (a soft
# S, an inky lifted black, a cream white), fine grain and a gentle vignette. On
# the colours: reds, yellows and blues drawn a little towards the logo's red,
# yellow and blue and made richer, greens and magentas calmer, ink-blue
# shadows and cream highlights. The tables are data (zifilm.py, made by
# gen/gen_zifilm.py), so every machine gets exactly the same numbers. All of it
# on the camera's own Y, U, V - no R,G,B picture in between (quick).
import zifilm as _zi  # noqa: E402

ZEROICHI_GRAIN = 9                         # the grain (Super 8: 12)
ZEROICHI_ROOM = 10                         # room left for it in the brightness
ZEROICHI_VIG = 0.22                        # the vignette (Hi-Con: 0.30)
ZI_TONE = list(_zi.TONE)                   # the film curve
T_ZI_YC = _compress(ZI_TONE, ZEROICHI_ROOM)                    # ... squeezed (room for the grain)
T_ZI_EXP = [int(min(255, max(0, round((v - ZEROICHI_ROOM) * 255 / (255 - 2 * ZEROICHI_ROOM)))))
            for v in range(256)]                               # ... and back
ZI_SY = (np.frombuffer(_zi.SY_LO, np.uint8).astype(np.int32)
         + 256 * np.frombuffer(_zi.SY_HI, np.uint8).astype(np.int32))
ZI_TU = np.frombuffer(_zi.TU, np.uint8).astype(np.int32) - 128
ZI_TV = np.frombuffer(_zi.TV, np.uint8).astype(np.int32) - 128
ZI_CU = np.frombuffer(_zi.CU, np.uint8).astype(np.int32) - 128
ZI_CV = np.frombuffer(_zi.CV, np.uint8).astype(np.int32) - 128


def _zi_luma(g):
    """The film's brightness: a Pillow L picture in, out (curve, grain, vignette)."""
    W0, H0 = g.size
    g = _point(g, T_ZI_YC)
    g = ImageChops.add_modulo(g, _grain_mod(W0, H0, ZEROICHI_GRAIN))
    g = ImageChops.multiply(g, _vig_mask(W0, H0, ZEROICHI_VIG, "L"))
    return _point(g, T_ZI_EXP)


def _zi_colour(T, U, V):
    """The film's colours: U, V (numpy) -> new U, V, by the graded brightness T
    at their size (the curve only - no grain). Whole numbers (as looks.c)."""
    T = np.asarray(T, np.intp)
    idx = ((np.asarray(U, np.intp) >> 1) << 7) | (np.asarray(V, np.intp) >> 1)
    s = ZI_SY[T]
    u = ((ZI_CU[idx] * s + 32896) >> 8) + ZI_TU[T]                # (+ 32896: rounded, never < 0)
    v = ((ZI_CV[idx] * s + 32896) >> 8) + ZI_TV[T]
    return np.clip(u, 0, 255).astype(np.uint8), np.clip(v, 0, 255).astype(np.uint8)


def zeroichi_yuv(Y, U, V):
    """A Zero Ichi picture from the camera's planes (numpy, U and V at half
    size): (Y, U, V) - photos straight to JPEG, video frames to the encoder."""
    u, v = zeroichi_colour(Y, U, V)
    return zeroichi_luma(Y), u, v


def zeroichi_luma(Y):
    """The live view's film brightness (numpy in, out) - its colours are in
    yuv565_table("Zero Ichi"), or zeroichi_colour."""
    return np.asarray(_zi_luma(Image.fromarray(np.ascontiguousarray(Y))))


def zeroichi_colour(Y, U, V):
    """The film's colours for U, V (half size) from the camera's Y (numpy)."""
    T = _point(Image.fromarray(np.ascontiguousarray(Y)), ZI_TONE)
    hs = (U.shape[1], U.shape[0])
    T = T.reduce(2) if T.size == (hs[0] * 2, hs[1] * 2) else T.resize(hs, Image.NEAREST)
    return _zi_colour(np.asarray(T), U, V)


def _gb_tone(img, pal, size=None):
    """Game Boy Camera: 128-pixel-wide sensor, harsh contrast with edge
    sharpening, and only 4 shades with its typical crosshatch dithering.
    Shrinks to the 128-pixel grid first, so almost all work is on a tiny image."""
    W0, H0 = size_of(img)
    gw = 128 if W0 >= H0 else max(8, round(128 * W0 / H0))
    gh = max(8, round(gw * H0 / W0))
    g = _shrink_fast(_img(img), gw, gh)
    g = g if g.mode == "L" else g.convert("L")
    g = _point(g.filter(_GB_SHARPEN), T_GB_CONTRAST)
    g = ImageChops.add(g, _bayer_img(gw, gh, "L", 85 * 0.95), 1.0, -128)
    tn = Tone(g, pal, size or (W0, H0))
    tn.sharp = True
    return tn


# one 3x3 sharpening pass: much lighter than an unsharp mask, similar crisp edges
_GB_SHARPEN = ImageFilter.Kernel((3, 3), (-1, -1, -1, -1, 12, -1, -1, -1, -1), scale=4)

TONE_FILTERS = {
    "Holga B&W": _holga_tone,
    "GB Camera": lambda img, size=None: _gb_tone(img, PAL_GB_GREEN, size),
    "GB Camera B&W": lambda img, size=None: _gb_tone(img, PAL_GB_GRAY, size),
    "Mono": _mono_tone,
    "Hi-Con B&W": _hicon_tone,
    "Thermal": _thermal_tone,
}


# --- filters (RGB in, RGB out) ----------------------------------------------
def normal(img):
    return img


def holga_bw(img):
    return _holga_tone(img).rgb()


def hicon_bw(img):
    return _hicon_tone(img).rgb()


def mono(img):
    return _mono_tone(img).rgb()


def thermal(img):
    return _thermal_tone(img).rgb()


def zero_ichi(img):
    """Zero Ichi on an R,G,B picture (GIFs): through Y, Cb, Cr at full size."""
    y, cb, cr = _pil(img).convert("YCbCr").split()
    u, v = _zi_colour(np.asarray(_point(y, ZI_TONE)), np.asarray(cb), np.asarray(cr))
    return Image.merge("YCbCr", (_zi_luma(y), Image.fromarray(u), Image.fromarray(v))).convert("RGB")


def gb_camera(img):
    return _gb_tone(img, PAL_GB_GREEN).rgb()


def gb_camera_bw(img):
    return _gb_tone(img, PAL_GB_GRAY).rgb()


def cross(img):
    return _point(_pil(img), T_CROSS)


# --- Holga Color: the Holga's plastic lens on colour film --------------------
# Sharp only in the middle, soft edges, very dark corners and the film border
# (as Holga B&W), with punchy contrast, strongly saturated, slightly warm colours
# and grain. Square, like the Holga's 6x6 film. Worked on the brightness and
# colour planes (YCbCr): contrast + grain on the brightness, saturation on
# the colour planes - a few table look-ups per pixel.
HOLGA_COL_Y = _lut(_sigmoid(7.0) * 1.03 - 4)     # punchy, deep blacks
T_HC_Y = _compress(HOLGA_COL_Y, HOLGA_GRAIN)      # (room for the grain)
T_HC_CB = [int(v) for v in _lut(128 + (_X - 128) * 1.65 - 7)]   # saturated, a little less blue
T_HC_CR = [int(v) for v in _lut(128 + (_X - 128) * 1.65 + 5)]   # saturated, a little warmer


def _hc_plane(p, k, lut, grain=None, lut2=None, vig=None, chroma=False):
    """One plane of Holga Color (an L picture): soft around (averaged down by
    k and enlarged again; 0: none), through lut - Y: + grain, then lut2 - then
    the vignette (Y darker; U, V towards grey, 128)."""
    w, h = p.size
    if k >= 2:
        p = Image.composite(p, p.reduce(k).resize((w, h), Image.BILINEAR), _focus_mask(w, h))
    p = _point(p, lut)
    if lut2 is not None:
        if grain is not None:
            p = ImageChops.add_modulo(p, grain)
        p = _point(p, lut2)
    if not chroma:
        return ImageChops.multiply(p, vig)
    c, v = np.asarray(p, np.int32), np.asarray(vig, np.int32)
    return Image.fromarray(((c * v + 128 * (255 - v) + 127) // 255).astype(np.uint8))


def _vig_min(w, h, uw, uh):
    """Holga Color's vignette at U, V's size: the darkest of each 2x2."""
    def make():
        a = np.asarray(_vig_mask(w, h, 0.55, power=2, gate=True))
        yy, xx = np.minimum(np.arange(2 * uh), h - 1), np.minimum(np.arange(2 * uw), w - 1)
        b = a[yy][:, xx].reshape(uh, 2, uw, 2).min(axis=(1, 3))
        return Image.fromarray(np.ascontiguousarray(b.astype(np.uint8)))
    return _cached(("vigmin", w, h, uw, uh), make)


def holga_color_yuv(Y, U, V):
    """(v55) Holga Color straight on the camera's planes (numpy; U, V half -
    or full - size) -> (Y, U, V): no R,G,B at all (until v54 the picture went
    to R,G,B, Y,Cb,Cr and back - about three times the work). Each plane at
    its own size: soft around (averaged down and enlarged), the tables (Y: contrast and grain; U, V:
    saturated, a little warm), the vignette and film border (Y darker; U, V
    towards grey: the same as darkening R, G and B - at U, V's size the
    darkest of each 2x2, so no colour on the border's edge)."""
    y, u, v = (Image.fromarray(np.ascontiguousarray(p)) for p in (Y, U, V))
    W0, H0 = y.size
    vig = _vig_mask(W0, H0, 0.55, power=2, gate=True)
    k = max(2, W0 // 110) if W0 > 110 else 0        # (soft: about 110-120 wide; U, V: the same k)
    yo = _hc_plane(y, k, T_HC_Y, _grain_mod(W0, H0, HOLGA_GRAIN_AMP), _EXPAND, vig)
    vc = vig if u.size == (W0, H0) else _vig_min(W0, H0, *u.size)
    uo = _hc_plane(u, k, T_HC_CB, vig=vc, chroma=True)
    vo = _hc_plane(v, k, T_HC_CR, vig=vc, chroma=True)
    return np.asarray(yo), np.asarray(uo), np.asarray(vo)


def holga_color(img):
    """Holga Color on an R,G,B picture (GIFs): through Y, Cb, Cr at full size."""
    y, cb, cr = _pil(img).convert("YCbCr").split()
    yo, uo, vo = holga_color_yuv(np.asarray(y), np.asarray(cb), np.asarray(cr))
    return Image.merge("YCbCr", [Image.fromarray(p) for p in (yo, uo, vo)]).convert("RGB")


# --- Prism: like shooting through a kaleidoscope prism filter ------------------
# (the faceted glass filters - "Kaleidoscope FX", "Kaleido"): a clear flat
# middle surrounded by slanted facets. Each facet bends the light, so it shows
# the picture shifted towards the middle: your subject repeats in a ring
# around itself, while the middle stays clear. The facet edges are soft (the
# filter sits right in front of the lens, out of focus), the glass splits the
# colours a little (rainbow edges on the copies), colours a little stronger.
# All in whole numbers, the same in the C Zero Ichi (looks.c).
# Sizes are relative to the picture: 1024 = half its shorter side.
#   nx, ny: the facets' directions x 256, in order around the middle
#   clear, ramp, most: the clear middle, its soft edge, how much a facet shows
#   shift: how far a facet moves the picture; disp: colour split (%)
#   soft: soft edges between neighbouring facets; sat: colour strength (256 = as is)
PRISMS = {
    # 6 facets (30, 90 ... 330 degrees): one copy above, one below, two each side
    "Prism": dict(nx=(222, 0, -222, -222, 0, 222), ny=(128, 256, 128, -128, -256, -128),
                  clear=500, ramp=300, most=180, shift=780, disp=4, soft=120, sat=280),
    # a triangle (90, 210, 330 degrees): 3 big copies - one below, two above
    "Prism 3": dict(nx=(0, -222, 222), ny=(256, -128, -128),
                    clear=420, ramp=260, most=190, shift=700, disp=4, soft=120, sat=280),
    # 6 facets, the colours split much more: rainbow edges everywhere
    "Prism Rainbow": dict(nx=(222, 0, -222, -222, 0, 222), ny=(128, 256, 128, -128, -256, -128),
                          clear=520, ramp=300, most=170, shift=780, disp=11, soft=120, sat=300),
}
_prism_cache = {}


def _tdiv(a, b):
    """a / b rounded towards 0 (as C does)."""
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b > 0) else -q


def _prism_layers(name, w, h):
    """What only depends on the size, per pixel: its facet (k1), the
    neighbouring facet it blends with (k2), how much of each (wa, wb), and how
    much of the facets shows there at all (r: 0 in the clear middle)."""
    key = (name, w, h)
    v = _prism_cache.get(key)
    if v is None:
        if len(_prism_cache) > 2:
            _prism_cache.clear()
        P = PRISMS[name]
        n = len(P["nx"])
        s = min(w, h)
        x = 2 * np.arange(w, dtype=np.int32) + 1 - w
        y = 2 * np.arange(h, dtype=np.int32) + 1 - h
        u = (np.abs(x) * 1024 // s * np.sign(x)).astype(np.int32)[None, :]
        v_ = (np.abs(y) * 1024 // s * np.sign(y)).astype(np.int32)[:, None]
        nx, ny = np.array(P["nx"], np.int32), np.array(P["ny"], np.int32)
        d1 = u * nx[0] + v_ * ny[0]
        k1 = np.zeros((h, w), np.int32)
        for k in range(1, n):                     # the facet: the most "facing" direction
            d = u * nx[k] + v_ * ny[k]
            better = d > d1
            d1 = np.where(better, d, d1)
            k1 = np.where(better, k, k1)
        kn, kp = (k1 + 1) % n, (k1 + n - 1) % n
        dn = u * nx[kn] + v_ * ny[kn]
        dp = u * nx[kp] + v_ * ny[kp]
        k2 = np.where(dn >= dp, kn, kp)
        g = np.minimum((d1 - np.maximum(dn, dp)) // P["soft"], 128)
        rho = np.maximum(d1, 0) >> 8
        r = np.minimum(np.maximum(rho - P["clear"], 0) * 256 // P["ramp"], P["most"])
        shifts = []                               # per colour, per facet: (dx, dy)
        for e in (-P["disp"], 0, P["disp"]):
            dc = P["shift"] * (100 + e) // 100
            shifts.append([(_tdiv(P["nx"][k] * dc * s, 256 * 2048), _tdiv(P["ny"][k] * dc * s, 256 * 2048))
                           for k in range(n)])
        v = _prism_cache[key] = (k1.astype(np.uint8), k2.astype(np.uint8), (128 + g).astype(np.int32),
                                 (128 - g).astype(np.int32), r.astype(np.int32), shifts, n)
    return v


def _prism(name, img):
    a = np.asarray(_pil(img))
    h, w = a.shape[:2]
    k1, k2, wa, wb, r, shifts, n = _prism_layers(name, w, h)
    ys, xs = np.arange(h), np.arange(w)
    masks = [(k1 == k, k2 == k) for k in range(n)]
    out = np.empty((h, w, 3), np.int32)
    for c in range(3):
        ac = a[..., c]
        A = np.zeros((h, w), np.uint8)
        B = np.zeros((h, w), np.uint8)
        for k, (m1, m2) in enumerate(masks):      # each facet: the picture, moved
            dx, dy = shifts[c][k]
            cp = ac[np.clip(ys - dy, 0, h - 1)][:, np.clip(xs - dx, 0, w - 1)]
            A = np.where(m1, cp, A)
            B = np.where(m2, cp, B)
        cp = (A.astype(np.int32) * wa + B.astype(np.int32) * wb) >> 8
        out[..., c] = (ac.astype(np.int32) * (256 - r) + cp * r) >> 8
    gr = (out[..., 0] * 77 + out[..., 1] * 150 + out[..., 2] * 29) >> 8
    out = gr[..., None] + ((out - gr[..., None]) * PRISMS[name]["sat"] >> 8)
    return Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))


def prism(img):
    return _prism("Prism", img)


def prism_3(img):
    return _prism("Prism 3", img)


def prism_rainbow(img):
    return _prism("Prism Rainbow", img)


EM_GRAIN = 24                     # room for the grain in the 8mm tone
EM_GRAIN_AMP = 12                 # how strong it is (v45: half)
_em_cache = {}


def _em_table_c(gain):
    """Super 8 colour table (flickering) squeezed to leave room for grain."""
    key = round(gain, 2)
    t = _em_cache.get(key)
    if t is None:
        base = _em_table(key)
        t = _em_cache[key] = [int(round(EM_GRAIN + v * (255 - 2 * EM_GRAIN) / 255)) for v in base]
    return t


S8_SAT = 310                       # Super 8: colours a little richer (256 = as is)
S8_LEAK = (255, 110, 30)           # the colour of a light leak (film burn)
_leak_cache = {}


def _s8_leak(w, side, strength):
    """A light leak from one side: per column, how much (0..256) - x strength."""
    key = (w, side, strength)
    v = _leak_cache.get(key)
    if v is None:
        if len(_leak_cache) > 8:
            _leak_cache.clear()
        x = np.arange(w, dtype=np.int32)
        if side:
            x = w - 1 - x
        t = np.maximum(256 - x * 384 // w, 0)
        v = _leak_cache[key] = ((t * t) >> 8) * strength >> 8
    return v


class _S8Film:
    """Super 8 is a moving film: in videos, GIFs and the live view what flickers,
    weaves and glows carries on from one frame to the next and changes gently -
    the brightness breathes a little, the picture drifts by a pixel now and
    then, a light leak glows up and fades over a second or two. (Photos: one
    frame of their own.) No scratches or dust (v48). Whole numbers and the
    same dice in the same order as the C Zero Ichi (looks.c, s8_next)."""
    k = 8                          # flicker: colour table 0..16 (brightness 0.90..1.06)
    dx = dy = 0                    # gate weave, pixels
    ln = ll = ls = lst = 0         # light leak: frames left, its length, side, strength


S8 = _S8Film()


def _s8_next(photo):
    """This frame's colour table, weave and light leak - see _S8Film."""
    f = S8
    if photo:                                          # a photo: one frame of its own
        gain = 0.90 + 0.16 * _pr.random()
        k = min(16, max(0, round(gain * 100) - 90))
        leak = (_pick(2), 170 + _pick(86)) if _pr.random() < 0.4 else None    # in 2 of 5
        return {"k": k, "dx": 0, "dy": 0, "leak": leak}
    f.k = (f.k * 3 + 4 + _pick(9) + 2) >> 2              # flicker: gently, 0.94..1.02
    if _pr.random() < 0.10:                            # weave: a pixel now and then
        f.dx = min(1, max(-1, f.dx + _pick(3) - 1))
    if _pr.random() < 0.06:
        f.dy = min(1, max(-1, f.dy + _pick(3) - 1))
    if f.ln == 0 and _pr.random() < 0.008:             # a light leak: glows up and fades
        f.ls, f.lst, f.ll = _pick(2), 170 + _pick(86), 10 + _pick(16)
        f.ln = f.ll
    leak = None
    if f.ln > 0:
        pos = f.ll - f.ln                              # 0 .. length-1
        e = min(256, min(pos + 1, f.ln) * 512 // f.ll)
        leak = (f.ls, f.lst * e >> 8)
        f.ln -= 1
    return {"k": f.k, "dx": f.dx, "dy": f.dy, "leak": leak}


def eightmm(img, out_size=None):
    """Super 8 home movie: warm, rich Kodachrome-like colours, a gentle flicker
    and weave, grain, now and then a light leak (film burn) and rounded
    film-gate corners (see _S8Film).
    Worked at the picture's own size up to 640 pixels wide (bigger: half
    size), photos at half their size, and enlarged smoothly at the end.
    (0.1.0: sharper - until then photos were softened to 170 pixels wide and
    worked at 320, clips at 320.)"""
    W0, H0 = size_of(img)
    OW, OH = out_size or (W0, H0)
    photo = OW > 640
    ww = OW // 2 if photo else (W0 // 2 if W0 > 640 else W0)
    wh = max(1, round(H0 * ww / W0))
    im = _pil(img)
    if photo and (ww, wh) != (W0, H0):
        im = im.resize((ww, wh), Image.BILINEAR)
    elif (ww, wh) != (W0, H0):
        im = _shrink_fast(im, ww, wh)
    fr = _s8_next(photo)
    if fr["dx"] or fr["dy"]:                           # gate weave (before the colours:
        im = ImageChops.offset(im, fr["dx"], fr["dy"])  # the same, per pixel)
    im = _point(im, _em_table_c(0.90 + fr["k"] * 0.01))     # colours + flicker
    a = np.asarray(im).astype(np.int32)                # richer colours
    gr = (a[..., 0] * 77 + a[..., 1] * 150 + a[..., 2] * 29) >> 8
    a = np.clip(gr[..., None] + (((a - gr[..., None]) * S8_SAT) >> 8), EM_GRAIN, 255 - EM_GRAIN)
    im = Image.fromarray(a.astype(np.uint8))
    im = ImageChops.add_modulo(im, _grain_mod(ww, wh, EM_GRAIN_AMP, "RGB"))   # grain
    leak = fr["leak"]
    if leak and leak[1] > 0:
        m = _s8_leak(ww, leak[0], leak[1])
        a = np.asarray(im).astype(np.int32)
        lc = (np.array(S8_LEAK, np.int32)[None, :] * m[:, None]) >> 8      # per column, per colour
        a = 255 - (255 - a) * (255 - lc[None]) // 255
        im = Image.fromarray(a.astype(np.uint8))
    im = ImageChops.multiply(im, _vig_mask(ww, wh, 0.55, "RGB", gate=True))   # last: clean corners
    if (ww, wh) != (OW, OH):
        im = im.resize((OW, OH), Image.BILINEAR)       # smoothly (no blocks)
    return im


def _gate_l(w, h):
    return _cached(("gate_l", w, h),
                   lambda: Image.fromarray((_gate(h, w)[..., 0] * 255).astype(np.uint8), "L"))


def _black(w, h):
    return _cached(("black", w, h), lambda: Image.new("RGB", (w, h), (0, 0, 0)))


# --- 16-Bit: pixel art ------------------------------------------------------
# Like a pixel-art camera: a small palette (see pixelart.py), chunky pixels
# (a 160 x 120 grid) and ordered dithering. It works straight on the camera's
# YUV picture with ready-made tables - a few table look-ups per pixel - so it
# is one of the lightest looks for the live view, videos and photos alike.
PIXEL_PALETTE = "Arcade"                 # (default palette)
_PIXEL_BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]],
                        np.float32)
_pixel_pals = {}


class _PixelPal:
    def __init__(self, name):
        import pixelart
        if name not in pixelart.PALETTES:
            name = pixelart.ORDER[0]
        cols = pixelart.colors(name)
        pad = cols + [cols[-1]] * (256 - len(cols))
        self.name = name
        self.pal = Palette(*([c[i] for c in pad] for i in range(3)))
        raw = pixelart.lut(name)
        self.lut = np.frombuffer(raw, np.uint8) if raw else _pixel_lut_slow(cols)
        self.amp = amp = pixelart.dither(name)
        v = np.arange(256 + 2 * amp)
        self.ty = ((np.clip(v - amp, 0, 255) >> 3) << 10).astype(np.uint16)
        q = np.arange(256)
        self.tu = ((q >> 3) << 5).astype(np.uint16)
        self.tv = (q >> 3).astype(np.uint16)
        # palette number -> R, G, B, 0 packed in 4 bytes: one look-up per pixel
        c = np.array(pad, np.uint32)
        self.rgb32 = (c[:, 0] | c[:, 1] << 8 | c[:, 2] << 16).astype("<u4")


def _pixel_lut_slow(cols):
    """Only if pixelart.py's table is damaged: a plain nearest-colour table
    (takes a few seconds on a Pi Zero)."""
    q = (np.arange(32, dtype=np.float32) * 8 + 4)
    pal = np.array(cols, np.float32)
    out = np.empty(32768, np.uint8)
    for yi in range(32):
        Y = q[yi]
        U, V = np.meshgrid(q - 128, q - 128, indexing="ij")
        rgb = np.stack([Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U], -1)
        rgb = np.clip(rgb, 0, 255).reshape(-1, 1, 3)
        d = ((rgb - pal[None]) ** 2 * np.array([0.3, 0.59, 0.11], np.float32)).sum(-1)
        out[yi * 1024:(yi + 1) * 1024] = d.argmin(1)
    return out


def _pixel_pal(name=None):
    name = name or PIXEL_PALETTE
    pp = _pixel_pals.get(name)
    if pp is None:
        pp = _pixel_pals[name] = _PixelPal(name)
    return pp


def pixel_grid(w, h):
    """The pixel-art grid for a w x h picture: 160 x 120 for 4:3, 120 x 120
    square (never more than the picture itself)."""
    s = min(1.0, 160 / max(w, h), 120 / min(w, h))
    return max(8, int(round(w * s))), max(8, int(round(h * s)))


def _dither_plane(w, h, amp):
    def make():
        t = np.tile(_PIXEL_BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
        return np.round((t + 0.5) * (2 * amp / 16.0)).astype(np.uint16)
    return _cached(("dither", w, h, amp), make)


def _pixel_planes(src, grid):
    """(Y, U, V, colour at half size?) of a picture at the grid size. `src`:
    the camera's (Y, U, V) planes, a YCbCr picture, or an RGB picture."""
    gw, gh = grid
    if isinstance(src, tuple):
        Y, U, V = src
        h, w = Y.shape
        if (w, h) == grid and U.shape == (gh // 2, gw // 2) and gw % 2 == 0 and gh % 2 == 0:
            return Y, U, V, True                       # live view: nothing to resize
        planes = [Image.fromarray(np.ascontiguousarray(p)) for p in (Y, U, V)]
        return tuple(np.asarray(_shrink_fast(p, gw, gh)) for p in planes) + (False,)
    im = _img(src) if not isinstance(src, Image.Image) else src
    if im.size != grid:
        im = _shrink_fast(im, gw, gh)
    if im.mode != "YCbCr":
        im = im.convert("RGB").convert("YCbCr")
    return tuple(np.asarray(p) for p in im.split()) + (False,)


def _pixel_index(src, palette=None):
    """(palette numbers on the grid, the palette, the picture's size).
    np.take: table look-ups, several times quicker than [] indexing."""
    if isinstance(src, tuple):
        H0, W0 = src[0].shape
    else:
        W0, H0 = size_of(src)
    grid = pixel_grid(W0, H0)
    Y, U, V, half = _pixel_planes(src, grid)
    pp = _pixel_pal(palette)
    d = _dither_plane(grid[0], grid[1], pp.amp)
    uv = np.take(pp.tu, U) + np.take(pp.tv, V)
    if half:                       # colour at half size: double it (each value twice
        uv = np.repeat((uv.astype(np.uint32) * 0x10001).view(np.uint16), 2, axis=0)  # + rows)
    idx = np.take(pp.lut, np.take(pp.ty, Y + d) + uv)
    return idx, pp, (W0, H0)


def _pixel_tone(src, size=None, palette=None):
    """The 16-Bit look as a Tone: palette numbers on the grid, enlarged (sharp
    pixel edges) to `size` when it becomes RGB / YUV / a GIF frame."""
    idx, pp, size0 = _pixel_index(src, palette)
    tn = Tone(Image.fromarray(np.ascontiguousarray(idx), "L"), pp.pal, size or size0,
              indexed=True)
    tn.sharp = True
    return tn


def pixel_tone_of(idx, pp):
    """The 16-Bit Tone for palette numbers `idx` (e.g. from the live view)."""
    tn = Tone(Image.fromarray(np.ascontiguousarray(idx), "L"), pp.pal, idx.shape[::-1],
              indexed=True)
    tn.sharp = True
    return tn


def pixel_rgb(src, palette=None, tone=False):
    """16-Bit for the live view: an R,G,B array at the grid size (quickest).
    tone=True: (that array, the Tone) - an instant video needs the Tone too."""
    idx, pp, size0 = _pixel_index(src, palette)
    rgb = np.take(pp.rgb32, idx).view(np.uint8).reshape(idx.shape + (4,))[..., :3]
    if not tone:
        return rgb
    tn = Tone(Image.fromarray(np.ascontiguousarray(idx), "L"), pp.pal, idx.shape[::-1],
              indexed=True)
    tn.sharp = True
    return rgb, tn


# The 16-Bit look comes in three palettes, each its own look in the filter list:
# "16-Bit Arcade", "16-Bit Neon", "16-Bit Paper" (see pixelart.py).
def _pixel_names():
    import pixelart
    return {f"16-Bit {p}": p for p in pixelart.ORDER}


PIXEL_LOOKS = _pixel_names()            # look name -> palette name


def pixel_palette_of(name):
    """The palette of a 16-Bit look ("16-Bit Neon" -> "Neon"), else None."""
    return PIXEL_LOOKS.get(name)


def _sixteen_bit(palette):
    def look(img, out_size=None):
        """16-Bit (pixel art) as an RGB picture (see _pixel_tone)."""
        return _pixel_tone(img, out_size, palette).rgb()
    return look


def _sixteen_bit_tone(palette):
    def tone(src, size=None, _palette=None):
        return _pixel_tone(src, size, palette)
    return tone


# Colour looks that work like the black & white ones (a Tone), but need the
# colour planes too: (Y, U, V) or an RGB picture in, Tone out.
COLOR_TONES = {name: _sixteen_bit_tone(p) for name, p in PIXEL_LOOKS.items()}


FILTERS = [
    ("Normal", normal),
    ("Super 8", eightmm),
    ("Holga B&W", holga_bw),
    ("Holga Color", holga_color),
    ("GB Camera", gb_camera),
    ("GB Camera B&W", gb_camera_bw),
    ("16-Bit Arcade", _sixteen_bit("Arcade")),
    ("16-Bit Neon", _sixteen_bit("Neon")),
    ("16-Bit Paper", _sixteen_bit("Paper")),
    ("Mono", mono),
    ("Hi-Con B&W", hicon_bw),
    ("Cross", cross),
    ("Thermal", thermal),
    ("Prism", prism),
    ("Prism 3", prism_3),
    ("Prism Rainbow", prism_rainbow),
    ("Zero Ichi", zero_ichi),            # (last: one press left from Normal)
]

SOFT_FOR_CLIPS = {}      # (no longer needed: the slow looks got fast enough)


def for_clip(name, fn, img):
    """Run filter `fn` on a GIF / video frame."""
    return fn(img)


# --- 16-bit screen: the camera's YUV straight to screen pixels ---------------
# For looks that only change each colour channel (Normal, Cross) the
# live view needs no R,G,B picture at all: one table turns the camera's Y, U, V
# (6 + 5 + 5 bits - as fine as the screen's own 16-bit colours can show) into
# the screen pixel, with the look already in it.
YUV_LOOKS = {"Normal": None, "Cross": (CROSS_R, CROSS_G, CROSS_B)}
_T_Y10 = ((np.arange(256) >> 2) << 10).astype(np.uint16)
_T_U5 = ((np.arange(256) >> 3) << 5).astype(np.uint16)
_T_V0 = (np.arange(256) >> 3).astype(np.uint16)
_yuv565 = {}


def yuv565_table(name):
    """(Y>>2, U>>3, V>>3) -> 16-bit screen pixel for look `name` (made once,
    about a tenth of a second on a Pi Zero)."""
    t = _yuv565.get(name)
    if t is None:
        yy = np.arange(64, dtype=np.float32) * 4 + 2
        cc = np.arange(32, dtype=np.float32) * 8 + 4 - 128
        Y, U, V = np.meshgrid(yy, cc, cc, indexing="ij")
        if name == "Zero Ichi":             # (its colours: the screen's brightness as T)
            u, v = _zi_colour(Y.astype(np.intp), (U + 128).astype(np.intp), (V + 128).astype(np.intp))
            U, V = u.astype(np.float32) - 128, v.astype(np.float32) - 128
        rgb = np.stack([Y + 1.402 * V, Y - 0.344136 * U - 0.714136 * V, Y + 1.772 * U], -1)
        rgb = np.clip(rgb + 0.5, 0, 255).astype(np.uint8).reshape(-1, 3)
        look = YUV_LOOKS.get(name)
        if look is not None:
            rgb = np.stack([np.asarray(look[i], np.uint8)[rgb[:, i]] for i in range(3)], -1)
        t = _yuv565[name] = rgb565(rgb)
    return t


def yuv_to_565(Y, U, V, table):
    """Camera planes (U, V at half size) -> 16-bit screen pixels, through a
    yuv565_table(): a few table look-ups per pixel."""
    uv = np.take(_T_U5, U)
    uv += np.take(_T_V0, V)
    idx = np.take(_T_Y10, Y)
    h, w = idx.shape
    if uv.shape == (h // 2, w // 2) and h % 2 == 0 and w % 2 == 0:
        uv = (uv.astype(np.uint32) * 0x10001).view(np.uint16)      # each value twice
        idx[0::2] += uv                                            # ... on both rows
        idx[1::2] += uv
    else:
        idx += np.asarray(Image.fromarray(uv).resize((w, h), Image.NEAREST))
    return np.take(table, idx)


# --- photos straight from the camera's YUV picture ----------------------------


def _yuv_rgb(Y, U, V, size=None):
    """YUV planes -> RGB Pillow picture (at `size`, shrinking first if smaller)."""
    h, w = Y.shape
    size = size or (w, h)
    planes = [Image.fromarray(np.ascontiguousarray(p)) for p in (Y, U, V)]
    planes = [p if p.size == size else p.resize(size, Image.BILINEAR, reducing_gap=2.0)
              for p in planes]
    return Image.merge("YCbCr", planes).convert("RGB")


def photo_from_yuv(name, Y, U, V, palette=None):
    """A finished photo from the camera's YUV planes (U, V at half size).
    Returns ("yuv", Y, U, V) - saved with no colour conversion at all - or
    ("img", picture) for the few looks that need RGB. Most looks avoid working
    on the full-size colour picture: black & white looks use the brightness
    plane only, Super 8 and 16-Bit start from a small picture."""
    h, w = Y.shape
    if name == "Normal":
        return ("yuv", Y, U, V)
    if name in TONE_FILTERS:
        return ("yuv",) + TONE_FILTERS[name](Image.fromarray(np.ascontiguousarray(Y)), (w, h)).yuv()
    if name == "Super 8":                       # (0.1.0: at half size - U, V's own)
        return ("img", eightmm(_yuv_rgb(Y, U, V, (w // 2, max(1, round(h * (w // 2) / w)))), (w, h)))
    if name in COLOR_TONES:
        return ("yuv",) + COLOR_TONES[name]((Y, U, V), (w, h)).yuv()
    if name == "Zero Ichi":
        return ("yuv",) + zeroichi_yuv(Y, U, V)
    if name == "Holga Color":                   # (v55: on Y, U, V too)
        return ("yuv",) + holga_color_yuv(Y, U, V)
    return ("img", dict(FILTERS).get(name, normal)(_yuv_rgb(Y, U, V)))


# These work at low resolution anyway, so when a video is developed its frames
# can be decoded at half size (JPEG does that almost for free) and the result
# enlarged once - same look, much less work.
HALF_SIZE_OK = {"Super 8", "GB Camera", "GB Camera B&W"} | set(PIXEL_LOOKS)


# Filters that were renamed or removed, for settings saved by older versions
OLD_NAMES = {"GameBoy": "GB Camera", "8-Bit": "16-Bit Arcade", "16-Bit": "16-Bit Arcade",
             "Harinezumi": "Normal", "Lomo": "Normal", "Sepia": "Mono", "Negative": "Normal",
             "Kaleidoscope": "Prism", "8mm": "Super 8", "GB Color": "GB Camera"}
# The filter list of v11 and earlier (settings stored the position in this list)
OLD_ORDER = ["Normal", "Harinezumi", "8mm", "Holga B&W", "Mono", "Sepia", "Lomo",
             "Cross", "GameBoy", "8-Bit", "Thermal", "Negative"]
