"""Reads zeroichi/settings.txt (key = value lines). Standard library only."""
import logging
import os
import re
from pathlib import Path

VERSION = "0.1.0"

# Where Zero Ichi lives on the SD card. (Set by zeroichi.py: these modules may be
# loaded from the compiled cache in /var/cache, so __file__ points there.)
APP_DIR = Path(os.environ.get("ZEROICHI_DIR") or Path(__file__).resolve().parent)
SETTINGS_FILE = APP_DIR / "settings.txt"
BOOL_KEYS = {"swap_rb", "allow_shutdown", "no_camera", "diagnostics", "optimize_boot",
             "fast_start"}
INT_KEYS = {"rotate", "spi_speed", "display_madctl", "spi_cs", "display_bits",
            "live_fps"}                        # "pin_..." are ints too
STR_KEYS = {"photo_dir", "display", "app", "photo_space"}
OPTIONAL_BOOL_KEYS = {"display_invert"}        # "auto" = use the display's default
DISPLAYS = ("1.3inch-hat", "2.8inch-hat", "st7789-320x240", "ili9341-320x240")
DISPLAY_ALIASES = {
    "2.8": "2.8inch-hat", "2.8inch": "2.8inch-hat", "2.8inch hat": "2.8inch-hat",
    "1.3": "1.3inch-hat", "1.3inch": "1.3inch-hat", "1.3inch hat": "1.3inch-hat",
    "waveshare": "1.3inch-hat", "hat": "1.3inch-hat", "st7789": "1.3inch-hat",
    "ili9341": "ili9341-320x240",
}


def pi_serial():
    """This Pi's serial number (each Zero Ichi's own screen is remembered by it)."""
    try:
        with open("/proc/device-tree/serial-number", "rb") as f:
            raw = f.read().decode(errors="replace")
    except OSError:
        raw = ""
    raw = os.environ.get("ZEROICHI_SERIAL", raw)
    return "".join(c for c in raw if c in "0123456789abcdefABCDEF")


def display_canon(name):
    """A display name as Zero Ichi knows it ("1.3inch-hat", "2.8inch-hat", ...).
    (Older versions called the 2.8inch hat "...-plus" / "...+".)"""
    v = str(name or "").strip().strip('"').lower()
    if v.endswith("+") or v.endswith("plus"):
        return "2.8inch-hat"
    return DISPLAY_ALIASES.get(v, v)
DEFAULTS = {"photo_dir": "auto", "rotate": 0, "swap_rb": False,
            "allow_shutdown": True, "no_camera": False, "diagnostics": False,
            "optimize_boot": True, "fast_start": True, "spi_speed": 40_000_000, "display": "1.3inch-hat",
            "display_madctl": None, "display_invert": None, "spi_cs": None,
            "display_bits": 16, "app": "c", "live_fps": None,
            "photo_space": "auto"}

log = logging.getLogger("zeroichi")


def read_settings(path=SETTINGS_FILE):
    """Read key = value lines from settings.txt (tolerates Windows line endings)."""
    out = {}
    try:
        text = path.read_text(encoding="utf-8-sig", errors="replace")
    except OSError:
        log.warning("No settings file at %s - using defaults", path)
        return out
    if text.lstrip().startswith("{\\rtf"):
        # Saved by TextEdit as "rich text": strip the formatting codes.
        log.warning("settings.txt was saved as rich text (RTF) - reading it anyway. "
                    "Tip: in TextEdit use Format > Make Plain Text.")
        text = re.sub(r"\\\r?\n", "\n", text)            # "\" at line ends
        text = re.sub(r"\\par[d]?\b", "\n", text)
        text = re.sub(r"\\'[0-9a-f]{2}|\\[a-z]+-?\d* ?|[{}]", "", text)
    lines = text.splitlines()
    for line in lines:
        line = line.split("#", 1)[0].strip()
        if "=" not in line:
            continue
        key, val = (x.strip() for x in line.split("=", 1))
        key = key.replace("-", "_").lower()
        if val.lower() in ("", "auto", "default"):
            continue                                   # keep the built-in default
        try:
            if key in BOOL_KEYS or key in OPTIONAL_BOOL_KEYS:
                out[key] = val.lower() in ("1", "yes", "true", "on")
            elif key in INT_KEYS or key.startswith("pin_"):
                out[key] = int(val, 0)                 # accepts 24 or 0x60
            elif key == "display":
                out[key] = display_canon(val)
            elif key in STR_KEYS:
                out[key] = val
            else:
                log.warning("Unknown setting %r ignored", key)
        except ValueError:
            log.warning("Bad value for %s: %r", key, val)
    return out


def settings(path=None):
    s = dict(DEFAULTS)
    s.update(read_settings(path or (APP_DIR / "settings.txt")))
    if s["display"] not in DISPLAYS:
        log.warning("Unknown display %r - using 1.3inch-hat", s["display"])
        s["display"] = "1.3inch-hat"
    return s
