"""Make zifilm.py: the tables of the Zero Ichi look (v52) - colour film with the
logo's colours. Worked out once here (floating point) and kept as data, so the
Python and C programs use exactly the same numbers on every machine.

usage: python3 gen_zifilm.py KIT_FOLDER     (writes KIT/zifilm.py)

The look, all on the camera's own Y, U, V (no R,G,B in between):
  brightness: a film curve - soft S, an inky lifted black, a cream white
              (TONE), then grain and a gentle vignette (see filters.zeroichi_*)
  colour:     reds, yellows and blues drawn a little towards the logo's red
              (220,58,40), yellow (246,184,26) and blue (28,76,156) and made
              richer; greens calmer and leaning to teal; magentas calmer
              (CU, CV: by the camera's U, V in steps of 2)
              ... less colour in the deepest shadows and brightest highlights
              (SY), ink-blue shadows and cream highlights (TU, TV) - both by
              the graded brightness
"""
import base64
import math
import os
import sys
import zlib

import numpy as np

KIT = sys.argv[1]

CONTRAST, BLACK, WHITE = 0.50, 0.09, 0.93         # the film curve
SAT = 1.18
# hue magnets: (hue in degrees on the U-V plane, width, pull towards it, colour gain)
MAGNETS = [(114, 18, 0.40, 1.38),                # the logo's red
           (154, 16, 0.40, 1.32),                # its yellow (oranges, skin's warm side)
           (328, 24, 0.40, 1.38),                # its blue
           (225, 30, 0.45, 0.62),                # greens: calmer
           (50, 25, 0.0, 0.80)]                  # magentas: calmer
SHADOW_DESAT, HI_DESAT = 0.35, 0.45
SHADOW_TINT, HI_TINT = (15, -9), (-13, 7)        # (U, V): ink blue / cream

x = np.arange(256) / 255.0
s = x * x * (3 - 2 * x)
tone = np.clip(np.round(255 * (BLACK + (WHITE - BLACK) * (x + CONTRAST * (s - x)))), 0, 255).astype(np.int64)

y = np.arange(256) / 255.0                       # (by the graded brightness)
sy = 1 - SHADOW_DESAT * np.clip(1 - y / 0.35, 0, 1) - HI_DESAT * np.clip((y - 0.75) / 0.25, 0, 1)
SY = np.round(sy * 256).astype(np.int64)
ws = np.clip(1 - y / 0.45, 0, 1) ** 1.5
wh = np.clip((y - 0.6) / 0.4, 0, 1) ** 1.5
TU = np.clip(np.round(128 + ws * SHADOW_TINT[0] + wh * HI_TINT[0]), 0, 255).astype(np.int64)
TV = np.clip(np.round(128 + ws * SHADOW_TINT[1] + wh * HI_TINT[1]), 0, 255).astype(np.int64)

q = np.arange(128) * 2 + 0.5 - 128               # the middle of each step of 2
cb, cr = np.meshgrid(q, q, indexing="ij")        # [U >> 1][V >> 1]
th = np.degrees(np.arctan2(cr, cb))
r = np.hypot(cb, cr)
shift = np.zeros_like(th)
gain = np.ones_like(th)
for h0, width, pull, g in MAGNETS:
    d = (h0 - th + 180) % 360 - 180
    w = np.exp(-0.5 * (d / width) ** 2)
    shift += w * pull * d
    gain += w * (g - 1)
th2 = np.radians(th + shift)
r2 = r * gain * SAT
CU = np.clip(np.round(128 + r2 * np.cos(th2)), 0, 255).astype(np.int64).reshape(-1)
CV = np.clip(np.round(128 + r2 * np.sin(th2)), 0, 255).astype(np.int64).reshape(-1)


def pack(name, a):
    raw = bytes(int(v) for v in a)
    s = base64.b64encode(zlib.compress(raw, 9)).decode()
    lines = "\n".join(f'    "{s[i:i + 76]}"' for i in range(0, len(s), 76))
    return f"{name} = _unpack(\n{lines}, {len(raw)}, {zlib.crc32(raw)})\n"


out = ['"""The Zero Ichi look\'s tables (v52) - made by src/zeroichi-c/gen/gen_zifilm.py',
       '(do not edit): see there for what each is. 256 values (by brightness) or',
       '128 x 128 (by the camera\'s U >> 1, V >> 1)."""',
       "import base64", "import zlib", "", "",
       "def _unpack(text, n, crc):",
       "    raw = zlib.decompress(base64.b64decode(text))",
       "    if len(raw) != n or zlib.crc32(raw) != crc:",
       "        raise ValueError(\"zifilm: damaged table\")",
       "    return raw", "", ""]
for name, a in (("TONE", tone), ("SY", SY), ("TU", TU), ("TV", TV), ("CU", CU), ("CV", CV)):
    if name == "SY":                              # (256 = 1.0: stored as 0..255 + the top bit below)
        assert a.max() <= 256 and a.min() >= 0
        out.append(pack("SY_LO", a & 255))
        out.append(pack("SY_HI", a >> 8))
    else:
        out.append(pack(name, a))
open(os.path.join(KIT, "zifilm.py"), "w").write("\n".join(out))
print("zifilm.py written; tone", tone[0], tone[128], tone[255], "SY", SY.min(), SY.max())
