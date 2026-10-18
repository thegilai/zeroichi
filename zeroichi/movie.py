"""
Minimal QuickTime (.mov) writer/reader for Photo-JPEG video.

Each frame is a JPEG; frames keep their real timestamps, so playback speed is
correct even though the Pi Zero's frame rate varies. Plays in QuickTime Player
(Mac), VLC and most video editors.
"""
import io
import os
import struct

TIMESCALE = 1000          # milliseconds

# What Zero Ichi writes into its photos, GIFs and videos: the camera, the
# program that made them, who made Zero Ichi and the look ("Look: Super 8") -
# no date (the Pi has no clock of its own: the time it carries on from can be
# wrong). The same as the C
# program's (src/zeroichi-c/meta.h).
META_MAKE = "Zero Ichi"                   # the camera's maker ...
META_MODEL = "Zero Ichi cam"              # ... and model
META_BY = "@thegilai"                     # who made Zero Ichi


def meta_software():
    """The program that made it, e.g. "Zero Ichi cam v0.1.0 by @thegilai"."""
    import config
    return f"Zero Ichi cam v{config.VERSION} by {META_BY}"


def look_text(look):
    """The look, as the description: "Look: Super 8"."""
    return f"Look: {look}"


def exif_segment(look=None):
    """A photo's EXIF (APP1): the look as its ImageDescription, Make, Model and
    Software - nothing else."""
    texts = ((((0x010E, look_text(look)),) if look else ()) +
             ((0x010F, META_MAKE), (0x0110, META_MODEL), (0x0131, meta_software())))
    off = 8 + 2 + 12 * len(texts) + 4                   # where the texts start
    ifd, data = [struct.pack(">H", len(texts))], b""
    for tag, text in texts:
        b = text.encode() + b"\0"
        if len(b) <= 4:                                 # (short: in the entry itself)
            ifd.append(struct.pack(">HHI4s", tag, 2, len(b), b))
            continue
        ifd.append(struct.pack(">HHII", tag, 2, len(b), off + len(data)))
        data += b + (b"\0" if len(b) & 1 else b"")
    tiff = b"MM\0\x2a" + struct.pack(">I", 8) + b"".join(ifd) + struct.pack(">I", 0) + data
    return b"\xff\xe1" + struct.pack(">H", 2 + 6 + len(tiff)) + b"Exif\0\0" + tiff


def with_exif(jpeg, look=None):
    """The JPEG with exif_segment(look) after its JFIF header (or its start)."""
    at = 2
    if jpeg[2:4] == b"\xff\xe0" and len(jpeg) >= 6:
        at = 4 + int.from_bytes(jpeg[4:6], "big")
    at = min(at, len(jpeg))
    return bytes(jpeg[:at]) + exif_segment(look) + bytes(jpeg[at:])


def gif_comment(look=None):
    return f"Made with {meta_software()}" + (f"\n{look_text(look)}" if look else "")

def _simplejpeg():
    """The fast JPEG library (installed with picamera2), or None. Looked up each
    time and checked to be the real thing (a real module has a file)."""
    try:
        import simplejpeg
    except Exception:
        return None
    return simplejpeg if getattr(simplejpeg, "__file__", None) else None


def _atom(kind, *payload):
    body = b"".join(payload)
    return struct.pack(">I4s", 8 + len(body), kind) + body


def _full(kind, version, flags, *payload):
    return _atom(kind, struct.pack(">I", (version << 24) | flags), *payload)


_MATRIX = struct.pack(">9I", 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)


def _matrix(rotation, w, h):
    """Display matrix: players turn the picture by this when showing it."""
    if rotation == 90:                    # 90 degrees clockwise
        return struct.pack(">9I", 0, 0x10000, 0, 0xFFFF0000, 0, 0, h << 16, 0, 0x40000000)
    return _MATRIX


_DHT = None


def _standard_huffman():
    """The standard JPEG Huffman tables (as written by libjpeg)."""
    global _DHT
    if _DHT is None:
        from PIL import Image
        b = io.BytesIO()
        Image.new("RGB", (16, 16)).save(b, "JPEG", quality=75)
        data, i, segs = b.getvalue(), 2, []
        while i + 4 <= len(data) and data[i] == 0xFF:
            marker, length = data[i + 1], int.from_bytes(data[i + 2:i + 4], "big")
            if marker == 0xC4:
                segs.append(data[i:i + 2 + length])
            if marker == 0xDA:
                break
            i += 2 + length
        _DHT = b"".join(segs)
    return _DHT


def needs_huffman(jpeg):
    """True if a JPEG frame leaves out its Huffman tables (hardware MJPEG does:
    they're implied) - they must be added for players and decoders."""
    head = bytes(jpeg[:4096])
    sos = head.find(b"\xff\xda")
    return sos >= 0 and b"\xff\xc4" not in head[:sos]


def with_huffman(jpeg):
    """The frame with the standard Huffman tables added if they're missing."""
    if not needs_huffman(jpeg):
        return jpeg
    return bytes(jpeg[:2]) + _standard_huffman() + bytes(jpeg[2:])


def huffman_parts(jpeg):
    """Like with_huffman(), but as pieces to write one after another - no copy
    of the (big) frame itself."""
    if not needs_huffman(jpeg):
        return (jpeg,)
    mv = memoryview(jpeg)
    return (mv[:2], _standard_huffman(), mv[2:])


def encode_jpeg(rgb, quality=75):
    """RGB numpy array -> JPEG bytes (simplejpeg if available, else Pillow)."""
    sj = _simplejpeg()
    if sj is not None:
        try:
            data = sj.encode_jpeg(rgb, quality=quality, colorspace="RGB", colorsubsampling="420")
            if isinstance(data, bytes):
                return data
        except Exception:
            pass
    from PIL import Image
    buf = io.BytesIO()
    Image.fromarray(rgb).save(buf, "JPEG", quality=quality)
    return buf.getvalue()


def encode_jpeg_yuv(y, u, v, quality=75):
    """Y, U, V numpy planes (U and V half size) -> JPEG bytes, with no colour
    conversion (JPEG stores YUV itself)."""
    import numpy as np
    sj = _simplejpeg()
    planes = [np.ascontiguousarray(p) for p in (y, u, v)]
    if sj is not None and hasattr(sj, "encode_jpeg_yuv_planes"):
        try:
            data = sj.encode_jpeg_yuv_planes(*planes, quality=quality)
            if isinstance(data, bytes):
                return data
        except Exception:
            pass
    from PIL import Image
    h, w = planes[0].shape
    ims = [Image.fromarray(planes[0])] + [Image.fromarray(p).resize((w, h)) for p in planes[1:]]
    buf = io.BytesIO()
    Image.merge("YCbCr", ims).save(buf, "JPEG", quality=quality)
    return buf.getvalue()


class MovWriter:
    def __init__(self, path, width, height, quality=80, rotation=0, look=None):
        self.path, self.w, self.h, self.quality = path, width, height, quality
        self.look = look                      # the look: the description "Look: ..." (None: none)
        self.rotation = rotation              # 0, or 90 = shown turned clockwise
        self.f = open(path, "wb", buffering=0)          # frames are written in one go
        self.pos = 0
        self._write((_atom(b"ftyp", b"qt  ", struct.pack(">I", 0x200), b"qt  "),))
        self.mdat_pos = self.pos
        self._write((struct.pack(">I4s", 0, b"mdat"),))    # size patched in close()
        self.sizes, self.offsets, self.times = [], [], []

    def _write(self, parts):
        """Write the pieces in order (one system call when possible)."""
        parts = [memoryview(p).cast("B") for p in parts]
        total = sum(p.nbytes for p in parts)
        done = os.writev(self.f.fileno(), parts) if hasattr(os, "writev") else 0
        if done < total:                       # partly written (rare): finish piece by piece
            skip = done
            for p in parts:
                if skip >= p.nbytes:
                    skip -= p.nbytes
                    continue
                p, skip = p[skip:], 0
                while p.nbytes:
                    p = p[self.f.write(p):]
        self.pos += total
        return total

    def add_jpeg(self, data, t):
        self.add_jpeg_parts((data,), t)

    def add_jpeg_parts(self, parts, t):
        """Add a frame given as pieces of a JPEG (see huffman_parts)."""
        for p in parts:
            if not isinstance(p, (bytes, bytearray, memoryview)):
                raise TypeError(f"frame is {type(p).__name__}, not JPEG bytes")
        pos = self.pos
        n = self._write(parts)                 # only count the frame once it's written
        self.offsets.append(pos)
        self.sizes.append(n)
        self.times.append(t)

    def add_frame(self, rgb, t):
        sj = _simplejpeg()
        if sj is not None:
            try:
                data = sj.encode_jpeg(rgb, quality=self.quality, colorspace="RGB",
                                      colorsubsampling="420")
            except Exception:
                data = None
            if isinstance(data, bytes):
                self.add_jpeg(data, t)
                return
        from PIL import Image
        buf = io.BytesIO()
        Image.fromarray(rgb).save(buf, "JPEG", quality=self.quality)
        self.add_jpeg(buf.getvalue(), t)

    def __len__(self):
        return len(self.sizes)

    def duration(self):
        return (self.times[-1] - self.times[0]) if len(self.times) > 1 else 0.0

    def close(self):
        end = self.pos
        self.f.seek(self.mdat_pos)
        self.f.write(struct.pack(">I", end - self.mdat_pos))
        self.f.seek(end)
        if self.sizes:
            self._write((self._moov(),))
        os.fsync(self.f.fileno())
        self.f.close()

    def _durations(self):
        ms = [max(1, round((b - a) * TIMESCALE)) for a, b in zip(self.times, self.times[1:])]
        ms.append(round(sum(ms) / len(ms)) if ms else 100)
        return ms

    def _moov(self):
        durs = self._durations()
        total = sum(durs)
        n = len(self.sizes)
        runs = []
        for d in durs:
            if runs and runs[-1][1] == d:
                runs[-1][0] += 1
            else:
                runs.append([1, d])

        mvhd = _full(b"mvhd", 0, 0, struct.pack(">IIII", 0, 0, TIMESCALE, total),
                     struct.pack(">IH", 0x10000, 0x100), b"\0" * 10, _MATRIX,
                     b"\0" * 24, struct.pack(">I", 2))
        tkhd = _full(b"tkhd", 0, 3, struct.pack(">IIIII", 0, 0, 1, 0, total), b"\0" * 8,
                     struct.pack(">HHHH", 0, 0, 0, 0), _matrix(self.rotation, self.w, self.h),
                     struct.pack(">II", self.w << 16, self.h << 16))
        mdhd = _full(b"mdhd", 0, 0, struct.pack(">IIIIHH", 0, 0, TIMESCALE, total, 0x55C4, 0))
        hdlr = _full(b"hdlr", 0, 0, b"mhlr", b"vide", b"\0" * 12, b"\x0cVideoHandler")
        vmhd = _full(b"vmhd", 0, 1, b"\0" * 8)
        dinf = _atom(b"dinf", _full(b"dref", 0, 0, struct.pack(">I", 1), _full(b"alis", 0, 1)))
        name = b"Photo - JPEG"
        entry = _atom(b"jpeg", b"\0" * 6, struct.pack(">H", 1),            # data ref 1
                      struct.pack(">HH4sII", 0, 0, b"appl", 0, 512),       # quality
                      struct.pack(">HHII", self.w, self.h, 72 << 16, 72 << 16),
                      struct.pack(">IH", 0, 1),                            # 1 frame/sample
                      bytes([len(name)]) + name + b"\0" * (31 - len(name)),
                      struct.pack(">hh", 24, -1))
        stsd = _full(b"stsd", 0, 0, struct.pack(">I", 1), entry)
        stts = _full(b"stts", 0, 0, struct.pack(">I", len(runs)),
                     b"".join(struct.pack(">II", c, d) for c, d in runs))
        stsc = _full(b"stsc", 0, 0, struct.pack(">IIII", 1, 1, 1, 1))
        stsz = _full(b"stsz", 0, 0, struct.pack(">II", 0, n), struct.pack(f">{n}I", *self.sizes))
        stco = _full(b"stco", 0, 0, struct.pack(">I", n), struct.pack(f">{n}I", *self.offsets))
        stbl = _atom(b"stbl", stsd, stts, stsc, stsz, stco)
        minf = _atom(b"minf", vmhd, dinf, stbl)
        trak = _atom(b"trak", tkhd, _atom(b"mdia", mdhd, hdlr, minf))
        texts = [(b"\xa9mak", META_MAKE), (b"\xa9mod", META_MODEL), (b"\xa9swr", meta_software())]
        if self.look:
            texts.append((b"\xa9cmt", look_text(self.look)))    # (the comment)
        udta = _atom(b"udta", *(_atom(k, struct.pack(">HH", len(t.encode()), 0x55C4), t.encode())
                                for k, t in texts))
        return _atom(b"moov", mvhd, trak, udta)            # (udta: who made it, the look)


class MovReader:
    """Reads movies written by MovWriter. Only the small index at the end of the
    file is read up front; each frame is read from the card when it's needed
    (reading the whole file first was slow for long videos)."""

    def __init__(self, path):
        self.path = path
        self._f = open(path, "rb")
        moov = self._top_level_moov()
        if moov is None:
            self._f.close()
            raise ValueError("not a finished movie (no index)")
        stbl = self._find(moov, 0, len(moov), [b"trak", b"mdia", b"minf", b"stbl"])
        if stbl is None:
            self._f.close()
            raise ValueError("no sample table")
        stsz = self._find(moov, *stbl, [b"stsz"])
        stco = self._find(moov, *stbl, [b"stco"])
        stts = self._find(moov, *stbl, [b"stts"])
        _, fixed, n = struct.unpack_from(">III", moov, stsz[0])
        self.sizes = ([fixed] * n if fixed else
                      list(struct.unpack_from(f">{n}I", moov, stsz[0] + 12)))
        (m,) = struct.unpack_from(">I", moov, stco[0] + 4)
        self.offsets = list(struct.unpack_from(f">{m}I", moov, stco[0] + 8))
        self.size = self._frame_size(moov)
        (r,) = struct.unpack_from(">I", moov, stts[0] + 4)
        self.durations = []
        for i in range(r):
            c, d = struct.unpack_from(">II", moov, stts[0] + 8 + 8 * i)
            self.durations += [d / TIMESCALE] * c

    def _top_level_moov(self):
        """Skip over the frames (mdat) to the index (moov) using the atom sizes."""
        f = self._f
        f.seek(0, os.SEEK_END)
        end = f.tell()
        pos = 0
        while pos + 8 <= end:
            f.seek(pos)
            size, kind = struct.unpack(">I4s", f.read(8))
            header = 8
            if size == 1:
                size = struct.unpack(">Q", f.read(8))[0]
                header = 16
            elif size == 0:
                size = end - pos
            if size < header:
                return None
            if kind == b"moov":
                return f.read(size - header)
            pos += size
        return None

    def _frame_size(self, moov):
        """Width x height (as stored) and display rotation, from the track header."""
        tkhd = self._find(moov, 0, len(moov), [b"trak", b"tkhd"])
        self.rotation = 0
        if tkhd is None:
            return (320, 240)
        w, h = struct.unpack_from(">II", moov, tkhd[1] - 8)
        a, b = struct.unpack_from(">ii", moov, tkhd[1] - 44)
        if a == 0 and b > 0:
            self.rotation = 90
        elif a == 0 and b < 0:
            self.rotation = 270
        elif a < 0:
            self.rotation = 180
        return (w >> 16, h >> 16)

    @staticmethod
    def _find(data, start, end, path):
        pos = start
        while pos + 8 <= end:
            size, kind = struct.unpack_from(">I4s", data, pos)
            if size < 8:
                return None
            if kind == path[0]:
                if len(path) == 1:
                    return (pos + 8, pos + size)
                return MovReader._find(data, pos + 8, pos + size, path[1:])
            pos += size
        return None

    def __len__(self):
        return len(self.sizes)

    def duration(self):
        return sum(self.durations)

    def jpeg(self, i):
        self._f.seek(self.offsets[i])
        return self._f.read(self.sizes[i])

    def frame(self, i, size=None):
        """Frame i as a Pillow image. With size=(w, h) the JPEG is decoded
        straight at (about) that size - much faster for big videos."""
        from PIL import Image
        im = Image.open(io.BytesIO(self.jpeg(i)))
        if size:
            im.draft("RGB", size)
        return im.convert("RGB")

    def frame_array(self, i, min_size=None):
        """Frame i as an RGB numpy array; with min_size=(w, h) decoded at a
        smaller size (but at least that big) - fast, for showing on the screen."""
        import numpy as np
        data = self.jpeg(i)
        sj = _simplejpeg()
        if sj is not None:
            try:
                kw = {"min_width": min_size[0], "min_height": min_size[1]} if min_size else {}
                a = sj.decode_jpeg(data, colorspace="RGB", **kw)
                if isinstance(a, np.ndarray) and a.ndim == 3:
                    return a
            except Exception:
                pass
        a = np.asarray(self.frame(i, min_size))
        if a.ndim != 3:
            raise ValueError(f"frame {i} could not be decoded")
        return a

    def frame_ycc(self, i, min_size=None):
        """Frame i as a Pillow "YCbCr" picture: JPEG's own colour planes, with no
        colour conversion, decoded small (at least min_size) when asked."""
        from PIL import Image
        im = Image.open(io.BytesIO(self.jpeg(i)))
        if min_size:
            im.draft("YCbCr", min_size)
        return im if im.mode == "YCbCr" else im.convert("RGB").convert("YCbCr")

    def frame_gray(self, i, min_size=None):
        """Frame i as brightness only (2-D numpy array) - decoding skips all the
        colour work, which is most of it."""
        import numpy as np
        data = self.jpeg(i)
        sj = _simplejpeg()
        if sj is not None:
            try:
                kw = {"min_width": min_size[0], "min_height": min_size[1]} if min_size else {}
                a = sj.decode_jpeg(data, colorspace="GRAY", **kw)
                if isinstance(a, np.ndarray) and a.ndim == 3:
                    return a[..., 0]
            except Exception:
                pass
        from PIL import Image
        im = Image.open(io.BytesIO(data))
        if min_size:
            im.draft("L", min_size)
        return np.asarray(im.convert("L"))

    def close(self):
        try:
            self._f.close()
        except Exception:
            pass

    def __del__(self):
        self.close()
