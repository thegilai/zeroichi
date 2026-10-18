"""
Minimal, fast driver for the Waveshare 1.3" LCD HAT (ST7789VW, 240x240, SPI).

Pinout (BCM) used by the HAT:
    DC  = 25    RST = 27    BL = 24    CS = CE0 (GPIO 8, handled by spidev)
"""
import fcntl
import os
import struct
import threading
import time

from gpio import OutputPin



def spi_buffer_size():
    """How many bytes the SPI driver takes per write (its 'bufsiz' setting).
    The default is only 4096: a 320x240 frame then needs 57 separate writes,
    each with its own set-up cost on a Pi Zero. Zero Ichi sets it to 65536."""
    try:
        with open("/sys/module/spidev/parameters/bufsiz") as f:
            return max(4096, int(f.read().strip()))
    except (OSError, ValueError):
        return 4096


class _RawSpi:
    """Tiny write-only stand-in for spidev using the kernel /dev/spidev API."""
    _WR_MODE, _WR_BITS, _WR_SPEED = 0x40016B01, 0x40016B03, 0x40046B04
    CHUNK = 4096             # replaced by the driver's real buffer size below

    def __init__(self, bus, dev, speed_hz):
        self.CHUNK = spi_buffer_size()
        self.fd = os.open(f"/dev/spidev{bus}.{dev}", os.O_RDWR)
        fcntl.ioctl(self.fd, self._WR_MODE, struct.pack("B", 0))
        fcntl.ioctl(self.fd, self._WR_BITS, struct.pack("B", 8))
        fcntl.ioctl(self.fd, self._WR_SPEED, struct.pack("I", speed_hz))

    def writebytes(self, data):
        self.writebytes2(bytes(data))

    def writebytes2(self, data):
        mv = memoryview(data).cast("B")     # os.write lets Python run meanwhile
        for i in range(0, len(mv), self.CHUNK):
            os.write(self.fd, mv[i:i + self.CHUNK])

    def close(self):
        os.close(self.fd)


# Supported displays. Pins are BCM numbers; every value can be overridden in
# settings.txt (display_madctl, display_invert, pin_dc, pin_rst, pin_bl, spi_cs).
PROFILES = {
    # Waveshare 1.3inch LCD HAT (the original Zero Ichi screen)
    "1.3inch-hat": dict(controller="st7789", width=240, height=240, madctl=0x70,
                        invert=True, dc=25, rst=27, bl=24, cs=0),
    # 2.8inch display hat: 320x240 ST7789, same pins and buttons as the
    # 1.3inch HAT. Its panel needs its own set-up ("st7789-2.8inch").
    "2.8inch-hat": dict(controller="st7789-2.8inch", width=320, height=240,
                        madctl=0x68, invert=True, dc=25, rst=27, bl=24, cs=0),
    # 320x240 ST7789 panels, e.g. Waveshare 2inch LCD Module
    "st7789-320x240": dict(controller="st7789", width=320, height=240, madctl=0x60,
                           invert=True, dc=25, rst=27, bl=18, cs=0),
    # 320x240 ILI9341 panels, e.g. Waveshare 2.4inch / 2.8inch LCD
    "ili9341-320x240": dict(controller="ili9341", width=320, height=240, madctl=0x28,
                            invert=False, dc=25, rst=27, bl=18, cs=0),
}


class ST7789:
    """SPI LCD driver for ST7789 and ILI9341 displays (240x240 or 320x240).
    (Kept under its old name so older code keeps working.)"""

    def __init__(self, spi_bus=0, spi_dev=None, dc=None, rst=None, bl=None,
                 speed_hz=40_000_000, rotate180=False, profile="1.3inch-hat",
                 madctl=None, invert=None, clear=True, bits=18, reset=True):
        p = dict(PROFILES.get(profile) or PROFILES["1.3inch-hat"])
        self.profile = profile if profile in PROFILES else "1.3inch-hat"
        self.controller = p["controller"]
        self.WIDTH, self.HEIGHT = p["width"], p["height"]
        self.madctl = p["madctl"] if madctl is None else madctl
        self.invert = p["invert"] if invert is None else invert
        spi_dev = p["cs"] if spi_dev is None else spi_dev
        self.spi_dev, self.speed_hz = spi_dev, speed_hz
        dev = f"/dev/spidev{spi_bus}.{spi_dev}"
        for _ in range(150):                 # early in boot the device may not exist yet
            if os.path.exists(dev):
                break
            time.sleep(0.1)
        self.spi = None
        if os.environ.get("ZEROICHI_SPI") != "spidev":
            try:
                self.spi = _RawSpi(spi_bus, spi_dev, speed_hz)
            except OSError:
                self.spi = None
        if self.spi is None:                 # fall back to the spidev library
            import spidev
            self.spi = spidev.SpiDev()
            self.spi.open(spi_bus, spi_dev)
            self.spi.max_speed_hz = speed_hz
            self.spi.mode = 0b00
        # Screen updates: one frame can be sent in the background while the
        # next one is being prepared (show_async).
        self._lock = threading.Lock()
        self._idle = threading.Event()
        self._idle.set()
        self._job = None
        self._wake = threading.Event()
        self._bufs, self._next, self._tmp, self._draw = [None, None], 0, None, None
        # Colour format for camera frames: 16-bit (RGB565, the default): a third
        # less to send than 18-bit, and the live view makes its pictures in
        # this format directly (small, then doubled), so it's also the
        # lightest. 18-bit takes R,G,B bytes as they are. Splash: always RGB565.
        self.bits = 18 if bits == 18 else 16
        self._colmod = 0x55
        threading.Thread(target=self._writer, daemon=True, name="lcd-writer").start()
        self.dc = OutputPin(p["dc"] if dc is None else dc)
        self.rst = OutputPin(p["rst"] if rst is None else rst, 1)
        self.bl = OutputPin(p["bl"] if bl is None else bl)
        self.rotate180 = rotate180
        self._closed = False
        # reset=False: the fast start's zeroichi-lcd has just set the panel up and
        # put the start-up picture on it - set it up again without the reset
        # pulse, which would blank the picture for a moment
        self._init_panel(reset)
        if clear:
            self.clear()
        self.bl.on()

    def helper_config(self):
        """Settings for zeroichi-lcd (the fast start's early start-up picture)."""
        return "".join(f"{k} {v}\n" for k, v in (
            ("controller", self.controller), ("width", self.WIDTH), ("height", self.HEIGHT),
            ("madctl", self.madctl), ("invert", int(bool(self.invert))), ("dc", self.dc.pin),
            ("rst", self.rst.pin), ("bl", self.bl.pin), ("cs", self.spi_dev),
            ("speed", self.speed_hz), ("flip", int(bool(self.rotate180)))))

    @classmethod
    def from_settings(cls, s, rotate180=False, clear=True, reset=True):
        """Create the display described by a settings dict (see config.py)."""
        return cls(profile=s.get("display", "1.3inch-hat"), speed_hz=s.get("spi_speed", 40_000_000),
                   bits=s.get("display_bits") or 16,
                   rotate180=rotate180, madctl=s.get("display_madctl"),
                   invert=s.get("display_invert"), dc=s.get("pin_dc"), rst=s.get("pin_rst"),
                   bl=s.get("pin_bl"), spi_dev=s.get("spi_cs"), clear=clear, reset=reset)

    # -- low level -------------------------------------------------------
    def _cmd(self, cmd, *data):
        self.dc.off()
        self.spi.writebytes([cmd])
        if data:
            self.dc.on()
            self.spi.writebytes(list(data))

    def _reset(self):
        self.rst.on()
        time.sleep(0.01)
        self.rst.off()
        time.sleep(0.01)
        self.rst.on()
        time.sleep(0.12)

    def _init_panel(self, reset=True):
        if reset:
            self._reset()
        if self.controller == "ili9341":
            self._init_ili9341()
        elif self.controller.startswith("st7789-"):    # the 2.8inch hat
            self._init_st7789_hat28()
            self._init_st7789_hat28()                  # twice, as its maker's driver does
        else:
            self._init_st7789()
        self._cmd(0x36, self.madctl)                   # MADCTL (orientation)
        self._cmd(0x21 if self.invert else 0x20)       # colour inversion on/off
        if not self.controller.startswith("st7789-"):  # (that sequence already did this)
            self._cmd(0x11)                            # sleep out
            time.sleep(0.12)
            self._cmd(0x29)                            # display on

    def _init_st7789(self):
        # Init sequence from Waveshare's reference code.
        self._cmd(0x3A, 0x05)                          # 16-bit RGB565
        self._cmd(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33)  # porch
        self._cmd(0xB7, 0x35)                          # gate control
        self._cmd(0xBB, 0x19)                          # VCOM
        self._cmd(0xC0, 0x2C)
        self._cmd(0xC2, 0x01)
        self._cmd(0xC3, 0x12)
        self._cmd(0xC4, 0x20)
        self._cmd(0xC6, 0x0F)                          # 60 Hz
        self._cmd(0xD0, 0xA4, 0xA1)                    # power
        self._cmd(0xE0, 0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F,
                  0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23)
        self._cmd(0xE1, 0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F,
                  0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23)

    def _init_st7789_hat28(self):
        # The 2.8inch hat's 320x240 ST7789 panel: the set-up from its maker's driver.
        self._cmd(0x11)                                # sleep out
        time.sleep(0.12)
        self._cmd(0x13)                                # normal display mode
        self._cmd(0xB6, 0x0A, 0x82)                    # display function control
        self._cmd(0x3A, 0x55)                          # 16-bit RGB565
        time.sleep(0.01)
        self._cmd(0xB2, 0x0C, 0x0C, 0x00, 0x33, 0x33)  # porch
        self._cmd(0xB7, 0x35)                          # gate control
        self._cmd(0xBB, 0x28)                          # VCOM
        self._cmd(0xC0, 0x0C)
        self._cmd(0xC2, 0x01, 0xFF)
        self._cmd(0xC3, 0x10)
        self._cmd(0xC4, 0x20)
        self._cmd(0xC6, 0x0F)
        self._cmd(0xD0, 0xA4, 0xA1)                    # power
        self._cmd(0xE0, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x32,
                  0x44, 0x42, 0x06, 0x0E, 0x12, 0x14, 0x17)
        self._cmd(0xE1, 0xD0, 0x00, 0x02, 0x07, 0x0A, 0x28, 0x31,
                  0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1B, 0x1E)
        self._cmd(0x21)                                # inversion on
        self._cmd(0x29)                                # display on
        time.sleep(0.12)

    def _init_ili9341(self):
        # Standard ILI9341 init sequence (as used by Adafruit / Waveshare).
        self._cmd(0xEF, 0x03, 0x80, 0x02)
        self._cmd(0xCF, 0x00, 0xC1, 0x30)
        self._cmd(0xED, 0x64, 0x03, 0x12, 0x81)
        self._cmd(0xE8, 0x85, 0x00, 0x78)
        self._cmd(0xCB, 0x39, 0x2C, 0x00, 0x34, 0x02)
        self._cmd(0xF7, 0x20)
        self._cmd(0xEA, 0x00, 0x00)
        self._cmd(0xC0, 0x23)                          # power control
        self._cmd(0xC1, 0x10)
        self._cmd(0xC5, 0x3E, 0x28)                    # VCOM
        self._cmd(0xC7, 0x86)
        self._cmd(0x3A, 0x55)                          # 16-bit RGB565
        self._cmd(0xB1, 0x00, 0x18)                    # frame rate
        self._cmd(0xB6, 0x08, 0x82, 0x27)
        self._cmd(0xF2, 0x00)
        self._cmd(0x26, 0x01)                          # gamma curve
        self._cmd(0xE0, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                  0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00)
        self._cmd(0xE1, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                  0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F)

    def _set_window(self, x0, y0, x1, y1):
        self._cmd(0x2A, x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF)
        self._cmd(0x2B, y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF)
        self._cmd(0x2C)

    # -- frame conversion ---------------------------------------------------
    def _to565(self, rgb):
        """RGB (H x W x 3, uint8) -> the screen's big-endian RGB565 bytes, using
        8-bit operations into reused buffers (fast on a Pi Zero)."""
        import numpy as np
        if self.rotate180:
            rgb = rgb[::-1, ::-1]
        h, w = rgb.shape[:2]
        i = self._next
        self._next ^= 1
        buf = self._bufs[i]
        if buf is None or buf.shape != (h, w, 2):
            buf = self._bufs[i] = np.empty((h, w, 2), np.uint8)
        if self._tmp is None or self._tmp.shape != (h, w):
            self._tmp = np.empty((h, w), np.uint8)
        tmp, hi, lo = self._tmp, buf[..., 0], buf[..., 1]
        r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
        np.bitwise_and(r, 0xF8, out=hi)                  # RRRRRGGG
        np.right_shift(g, 5, out=tmp)
        np.bitwise_or(hi, tmp, out=hi)
        np.left_shift(g, 3, out=tmp)                     # GGGBBBBB
        np.bitwise_and(tmp, 0xE0, out=tmp)
        np.right_shift(b, 3, out=lo)
        np.bitwise_or(lo, tmp, out=lo)
        return buf

    def _set_colmod(self, value):
        """0x55 = 16-bit RGB565, 0x66 = 18-bit (3 bytes per pixel)."""
        if self._colmod != value:
            self._cmd(0x3A, value)
            self._colmod = value

    def _frame_bytes(self, rgb):
        """Camera frame -> what the screen is sent, in the current colour format."""
        import numpy as np
        if self.bits == 16:
            return self._to565(rgb), 0x55
        if self.rotate180:
            rgb = rgb[::-1, ::-1]
        h, w = rgb.shape[:2]
        i = self._next
        self._next ^= 1
        buf = self._bufs[i]
        if buf is None or buf.shape != (h, w, 3):
            buf = self._bufs[i] = np.empty((h, w, 3), np.uint8)
        np.copyto(buf, rgb)                           # one plain copy, no conversion
        return buf, 0x66

    def _write_full(self, buf, colmod=0x55):
        if not isinstance(buf, (bytes, bytearray)):
            buf = memoryview(buf).cast("B")            # flat bytes for any SPI backend
        with self._lock:
            self._set_colmod(colmod)
            self._set_window(0, 0, self.WIDTH - 1, self.HEIGHT - 1)
            self.dc.on()
            self.spi.writebytes2(buf)

    def _writer(self):
        while True:
            self._wake.wait()
            self._wake.clear()
            job, self._job = self._job, None
            if job is not None:
                try:
                    self._write_full(*job)
                except Exception:
                    pass
            self._idle.set()

    def wait(self):
        """Wait until the last background frame has been sent."""
        self._idle.wait()

    # -- public ---------------------------------------------------------------
    def show(self, rgb):
        """Push a full-screen HxWx3 uint8 RGB numpy array to the panel (waits)."""
        buf, colmod = self._frame_bytes(rgb)
        self.wait()
        self._write_full(buf, colmod)

    def show_async(self, rgb):
        """Like show(), but sends the frame in the background so the caller can
        prepare the next one meanwhile (only one frame is ever in flight)."""
        buf, colmod = self._frame_bytes(rgb)  # into the buffer that isn't being sent
        self.wait()
        self._idle.clear()
        self._job = (buf, colmod)
        self._wake.set()

    def frame_buffer(self):
        """The buffer the next frame can be drawn straight into - sent exactly
        as it is, no conversion: H x W x 2 bytes of big-endian RGB565 (16-bit
        colour) or H x W x 3 R,G,B bytes (18-bit). (Flipped screen: a separate
        drawing buffer, turned while it is copied into the send buffer.)"""
        import numpy as np
        shape = (self.HEIGHT, self.WIDTH, 2 if self.bits == 16 else 3)
        if self.rotate180:
            if self._draw is None or self._draw.shape != shape:
                self._draw = np.zeros(shape, np.uint8)
            return self._draw
        i = self._next
        buf = self._bufs[i]
        if buf is None or buf.shape != shape:
            buf = self._bufs[i] = np.zeros(shape, np.uint8)
        return buf

    def show_buffer_async(self, buf):
        """Send a frame drawn into frame_buffer() (in the background)."""
        import numpy as np
        i = self._next
        if self.rotate180 or buf is not self._bufs[i]:
            # (the send buffer that is free: the other one may still be in flight)
            out = self._bufs[i]
            if out is None or out.shape != buf.shape:
                out = self._bufs[i] = np.empty(buf.shape, np.uint8)
            np.copyto(out, buf[::-1, ::-1] if self.rotate180 else buf)
            buf = out
        self._next ^= 1                             # (the other buffer is the one in flight)
        self.wait()
        self._idle.clear()
        self._job = (buf, 0x55 if buf.shape[2] == 2 else 0x66)
        self._wake.set()

    def show_bytes(self, buf):
        """Push a raw full-screen big-endian RGB565 buffer (no numpy needed)."""
        if self.rotate180:
            px = [buf[i:i + 2] for i in range(len(buf) - 2, -1, -2)]
            buf = b"".join(px)
        self.wait()
        self._write_full(buf)

    def show_region(self, x0, y0, x1, y1, buf):
        """Update just a rectangle (inclusive coords) with raw RGB565 bytes of
        one colour. Used for the start-up progress bar."""
        if self.rotate180:
            x0, x1 = self.WIDTH - 1 - x1, self.WIDTH - 1 - x0
            y0, y1 = self.HEIGHT - 1 - y1, self.HEIGHT - 1 - y0
        self.wait()
        with self._lock:
            self._set_colmod(0x55)
            self._set_window(x0, y0, x1, y1)
            self.dc.on()
            self.spi.writebytes2(buf)

    def sleep(self):
        """Screen saver: backlight off and the panel asleep (draws almost no
        power). wake() brings it back exactly as it was."""
        self.wait()
        with self._lock:
            self.bl.off()
            self._cmd(0x28)                            # display off
            self._cmd(0x10)                            # sleep in
        self.asleep = True

    def wake(self):
        if not getattr(self, "asleep", False):
            return
        with self._lock:
            self._cmd(0x11)                            # sleep out
            time.sleep(0.12)                           # (the panel needs 120 ms)
            self._cmd(0x29)                            # display on
        self.asleep = False

    def backlight_on(self):
        self.bl.on()

    def release(self):
        """Free SPI/GPIO but leave the picture and backlight as they are."""
        if self._closed:
            return
        self.wait()
        self._closed = True
        self.spi.close()
        for pin in (self.dc, self.rst, self.bl):
            try:
                pin.close()
            except Exception:
                pass

    def show_pil(self, img):
        import numpy as np
        self.show(np.asarray(img.convert("RGB")))

    def clear(self, color=(0, 0, 0)):
        r, g, b = color
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        self.show_bytes(bytes((v >> 8, v & 0xFF)) * (self.WIDTH * self.HEIGHT))

    def close(self):
        if self._closed:
            return
        self._closed = True
        try:
            self.clear()
            self.bl.off()
            self._cmd(0x28)   # display off
            self._cmd(0x10)   # sleep in
        finally:
            self.spi.close()
