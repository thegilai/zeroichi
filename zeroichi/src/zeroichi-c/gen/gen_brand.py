"""Make Zero Ichi's start-up pictures and the screen saver's artwork (v51).

usage: python3 gen_brand.py KIT_FOLDER [PREVIEW_FOLDER]

Writes, from the Bauhaus design (start-up "S", screen saver "W"):
  KIT/splash.rgb565, KIT/splash-320x240.rgb565   the start-up pictures (16-bit
                                                  big-endian RGB565, with the version)
  KIT/brandart.py                                 the screen saver's two pictures
  KIT/src/zeroichi-c/brandart.c                     ... the same, for the C program
The screen saver's moving shapes are drawn by the camera itself (screensaver.c,
app.py) - only the sleeping "0 1" and the line of text are pictures, because the
camera has no Poppins font. Needs Pillow, Poppins (Google Fonts) and Noto Sans CJK.
"""
import base64
import os
import sys
import zlib

from PIL import Image, ImageDraw, ImageFont

KIT = sys.argv[1]
PREVIEW = sys.argv[2] if len(sys.argv) > 2 else None
sys.path.insert(0, KIT)
import config  # noqa: E402

POP = "/usr/share/fonts/truetype/google-fonts/Poppins-Bold.ttf"
POPM = "/usr/share/fonts/truetype/google-fonts/Poppins-Medium.ttf"
CJK = "/usr/share/fonts/opentype/noto/NotoSansCJK-Black.ttc"
CREAM, PAPER, RED, YEL, BLU, BLK = (242, 234, 216), (232, 222, 200), (220, 58, 40), (246, 184, 26), (28, 76, 156), (24, 24, 26)
GREY = (120, 112, 98)
SS = 4                                   # drawn 4 times bigger, then shrunk: smooth edges


class C:
    """a canvas drawn SS times bigger (bg None: transparent)"""
    def __init__(self, w, h, bg):
        self.w, self.h = w, h
        self.im = Image.new("RGBA", (w * SS, h * SS), bg + (255,) if bg else (0, 0, 0, 0))
        self.d = ImageDraw.Draw(self.im)

    @staticmethod
    def p(v):
        return v * SS

    def circle(self, cx, cy, r, fill):
        p = self.p
        self.d.ellipse([p(cx - r), p(cy - r), p(cx + r), p(cy + r)], fill=fill)

    def rect(self, x0, y0, x1, y1, fill):
        p = self.p
        self.d.rectangle([p(x0), p(y0), p(x1), p(y1)], fill=fill)

    def poly(self, pts, fill):
        self.d.polygon([(self.p(x), self.p(y)) for x, y in pts], fill=fill)

    def pie(self, cx, cy, r, a0, a1, fill):
        p = self.p
        self.d.pieslice([p(cx - r), p(cy - r), p(cx + r), p(cy + r)], a0, a1, fill=fill)

    def line(self, x0, y0, x1, y1, fill, w):
        p = self.p
        self.d.line([(p(x0), p(y0)), (p(x1), p(y1))], fill=fill, width=max(1, int(p(w))))

    def text(self, x, y, t, size, fill, anchor="mm", font=POP, spacing=0.0):
        f = ImageFont.truetype(font, int(self.p(size)), index=0)
        if not spacing:
            self.d.text((self.p(x), self.p(y)), t, font=f, fill=fill, anchor=anchor)
            return
        widths = [f.getlength(ch) for ch in t]           # letter-spaced, one letter at a time
        total = sum(widths) + self.p(spacing) * (len(t) - 1)
        sx = self.p(x) - (total / 2 if anchor[0] == "m" else total if anchor[0] == "r" else 0)
        for ch, wd in zip(t, widths):
            self.d.text((sx, self.p(y)), ch, font=f, fill=fill, anchor="l" + anchor[1])
            sx += wd + self.p(spacing)

    def eye(self, x, y, r):
        self.circle(x, y, r, BLK)
        self.circle(x - r * .32, y - r * .35, r * .36, CREAM)

    def done(self):
        return self.im.reduce(SS)                          # (box: no dark fringes)


def mark01(c, x, y, s):
    """the mark: a red ring (0), a black flagged bar (1), the yellow quarter and
    the blue square; Zero peeking with two little eyes"""
    r = 60 * s
    c.circle(x, y, r, RED)
    c.circle(x, y, r * .40, CREAM)
    bx = x + r + 22 * s
    c.rect(bx, y - r, bx + 30 * s, y + r, BLK)
    c.poly([(bx, y - r), (bx, y - r + 30 * s), (bx - 26 * s, y - r + 30 * s)], BLK)
    c.pie(bx + 30 * s, y + r, 42 * s, 270, 360, YEL)
    c.rect(bx + 46 * s, y + r * .24, bx + 76 * s, y + r * .24 + 30 * s, BLU)
    for ex in (-14, 14):
        c.eye(x + ex * s, y - r * .70, 5.2 * s)


def startup(w, h):
    """S: the start-up picture, with the version and screen size at the bottom right"""
    c = C(w, h, CREAM)                     # (v54: plain cream - no thin lines)
    wide = w > h
    s = .92 if wide else .78
    mark01(c, w * (.36 if wide else .34), h * (.38 if wide else .36), s)
    c.text(w * .125, h * .83, "zero ichi", 27 if wide else 24, BLK, anchor="lm")
    c.text(w * .875, h * .84, "ゼロイチ", 12 if wide else 11, RED, anchor="rm", font=CJK)
    c.text(w * .125, h * .94, "starting", 9, GREY, anchor="lm", font=POPM, spacing=.5)
    c.text(w * .875, h * .94, f"v{config.VERSION} · {w}x{h}", 9, GREY, anchor="rm", font=POPM)
    return c.done().convert("RGB")


# W: the screen saver's sleeping "0 1" (with its dark halo, so the drifting
# shapes pass behind it) and its line of text - transparent pictures, placed
# relative to the screen's middle (both screens: the same pictures)
SC = .42


def saver_mark(w=320, h=240):
    c = C(w, h, None)
    x, y, r = w / 2 - 26 * SC, h / 2 - 4, 60 * SC
    c.circle(x, y, r + 5, BLK)
    c.rect(x + r + 22 * SC - 5, y - r - 5, x + r + 52 * SC + 5, y + r + 5, BLK)
    c.circle(x, y, r, RED)
    c.circle(x, y, r * .40, BLK)
    bx = x + r + 22 * SC
    c.rect(bx, y - r, bx + 30 * SC, y + r, CREAM)
    c.poly([(bx, y - r), (bx, y - r + 30 * SC), (bx - 26 * SC, y - r + 30 * SC)], CREAM)
    p = c.p
    for ex in (-14, 14):                                 # asleep: closed eyes
        c.d.arc([p(x + ex * SC - 5), p(y - r * .70 - 3), p(x + ex * SC + 5), p(y - r * .70 + 4)], 20, 160,
                fill=BLK, width=int(p(2)))
    c.text(x + r + 70 * SC, y - r - 2, "z", 10, CREAM, font=POPM)
    c.text(x + r + 86 * SC, y - r - 13, "z", 8, (170, 170, 170), font=POPM)
    return c.done()


def saver_text(w=320, h=240):
    c = C(w, h, None)
    c.text(w / 2, h - 14, "zero ichi  ·  press any key", 9, (120, 120, 120), font=POPM, spacing=.5)
    return c.done()


def sprite(im, w=320, h=240, ref_y=None):
    """crop to what's drawn: (dx, dy, w, h, RGBA bytes), dx/dy from the middle
    of the screen (x) and from ref_y (y; default the middle)"""
    x0, y0, x1, y1 = im.getchannel("A").getbbox()
    cut = im.crop((x0, y0, x1, y1))
    ry = h // 2 if ref_y is None else ref_y
    return (x0 - w // 2, y0 - ry, x1 - x0, y1 - y0, cut.tobytes())


def rgb565_be(im):
    d = im.tobytes()
    out = bytearray()
    for i in range(0, len(d), 3):
        r, g, b = d[i], d[i + 1], d[i + 2]
        v = (r & 0xF8) << 8 | (g & 0xFC) << 3 | b >> 3
        out += bytes((v >> 8, v & 255))
    return bytes(out)


def kana(w=120, h=30):
    """(v53) the name in katakana, for MENU > About (the camera has no such font)"""
    c = C(w, h, None)
    c.text(4, h / 2, "ゼロイチ", 11, RED, anchor="lm", font=CJK)
    im = c.done()
    x0, y0, x1, y1 = im.getchannel("A").getbbox()
    return (0, 0, x1 - x0, y1 - y0, im.crop((x0, y0, x1, y1)).tobytes())


mark = sprite(saver_mark())
text = sprite(saver_text(), ref_y=240)                  # (from the bottom edge)
kana_sp = kana()
for w, h, name in ((240, 240, "splash.rgb565"), (320, 240, "splash-320x240.rgb565")):
    im = startup(w, h)
    open(os.path.join(KIT, name), "wb").write(rgb565_be(im))
    if PREVIEW:
        im.save(os.path.join(PREVIEW, name + ".png"))

# Python: brandart.py
def b64(data):
    s = base64.b64encode(zlib.compress(data, 9)).decode()
    return "\n".join(f'    "{s[i:i + 76]}"' for i in range(0, len(s), 76))


py = ['"""Zero Ichi\'s screen saver artwork - made by src/zeroichi-c/gen/gen_brand.py (do not edit).',
      'MARK, TEXT: (dx, dy, w, h, RGBA bytes): placed at dx from the middle of the screen, dy from',
      'its middle (MARK) or its bottom edge (TEXT). KANA (v53, MENU > About): the name in',
      'katakana (dx, dy: 0)."""', "import base64", "import zlib", "", ""]
for nm, sp in (("MARK", mark), ("TEXT", text), ("KANA", kana_sp)):
    py.append(f"{nm} = ({sp[0]}, {sp[1]}, {sp[2]}, {sp[3]}, zlib.decompress(base64.b64decode(")
    py.append(b64(sp[4]) + ")))")
    py.append("")
open(os.path.join(KIT, "brandart.py"), "w").write("\n".join(py))

# C: brandart.c
cs = ["/* Zero Ichi's screen saver artwork - made by gen/gen_brand.py (do not edit).",
      " * (dx, dy): from the middle of the screen (MARK) or the middle of its bottom edge (TEXT);",
      " * KANA (v53, MENU > About): the name in katakana */",
      '#include "brandart.h"', ""]
for nm, sp in (("MARK", mark), ("TEXT", text), ("KANA", kana_sp)):
    data = sp[4]
    rows = [", ".join(str(b) for b in data[i:i + 32]) for i in range(0, len(data), 32)]
    cs.append(f"const BrandArt BR_{nm} = {{{sp[0]}, {sp[1]}, {sp[2]}, {sp[3]}, (const uint8_t[]){{\n    "
              + ",\n    ".join(rows) + "\n}};")
open(os.path.join(KIT, "src", "zeroichi-c", "brandart.c"), "w").write("\n".join(cs) + "\n")
print("mark", mark[:4], "text", text[:4], "kana", kana_sp[:4])
if PREVIEW:
    saver_mark().save(os.path.join(PREVIEW, "saver-mark.png"))
    saver_text().save(os.path.join(PREVIEW, "saver-text.png"))
