"""
Colour palettes for the 16-Bit look - pixel art like a Pixless-style camera:
a small set of colours, chunky pixels and ordered dithering.

For each palette there is a ready-made table saying which palette colour each
camera colour becomes (worked out once on a PC, with a little extra colour and
contrast and a perceptual colour match - that takes seconds on a Pi Zero, but
using the table takes milliseconds). The table is indexed by the camera's own
YUV values: (Y >> 3) << 10 | (U >> 3) << 5 | (V >> 3).
"""
import base64
import zlib

ORDER = ["Arcade", "Neon", "Paper"]

PALETTES = {
    "Arcade": {
        "colors": (
        "0b0a12 2a2836 54505f 8a8594 c2bec6 f5f2ea 4a3c36 7d6a5e "
        "b3a08c dccdb6 3a1d18 6b3a25 a3602f d6914f f2c58f 5e1428 "
        "a82a3b e3533c f39a8c f0952a f7d348 fdf3a0 12301e 22603a "
        "3c9446 85c64a cfe98f 124c58 268a8c 6dd0cc 141a42 26428e "
        "3f74d4 7fb3ef c9e4f9 3b1e52 70409a b875c2 "
        ),
        "dither": 24,
        "crc": 720911574,
        "lut": (
        "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"
        ),
    },
    "Neon": {
        "colors": (
        "0d0221 241046 3b1c6e 5d2a9c 2a2f7a 1f5fbf 18a6e0 5ef2ff "
        "d6fbff ff2e88 ff6fb5 b3196e ff8a3d ffd23f 7a1f3d f9f0ff "
        ),
        "dither": 32,
        "crc": 2812735634,
        "lut": (
        "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"
        "dzLuZNzJuEO4fw13J/g/KVN1Pg=="
        ),
    },
    "Paper": {
        "colors": (
        "f2e8d5 dfd0b3 b9a787 8b7a63 5a4c3e 2c2420 b8574a d99a6c "
        "5e7f97 9db3c2 7d9a6a c8a84b "
        ),
        "dither": 28,
        "crc": 3405354040,
        "lut": (
        "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"
        ),
    },
}


def names():
    return list(ORDER)


def colors(name):
    """[(r, g, b), ...] of a palette."""
    s = "".join(PALETTES[name]["colors"]).split()
    return [tuple(int(c[i:i + 2], 16) for i in (0, 2, 4)) for c in s]


def dither(name):
    """How strong the ordered dithering is (brightness steps)."""
    return PALETTES[name]["dither"]


def lut(name):
    """The 32768-entry colour table (bytes), or None if it's damaged."""
    p = PALETTES[name]
    try:
        raw = zlib.decompress(base64.b64decode("".join(p["lut"])))
    except Exception:
        return None
    if len(raw) != 32768 or zlib.crc32(raw) != p["crc"]:
        return None
    return raw
