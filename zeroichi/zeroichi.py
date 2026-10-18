#!/usr/bin/env python3
"""
Zero Ichi launcher - kept small so the splash screen appears as fast as possible.

  1. show the splash screen (needs only the display driver)
  2. start the camera in the background (camera.py)
  3. meanwhile load numpy / Pillow / the camera app
  4. hand over to the app (app.py)

Zero Ichi's files live on the SD card's boot partition, /boot/firmware/zeroichi, so
you can edit them from a computer. After the one-time setup the Pi starts a copy
in /var/lib/zeroichi/app instead: the boot partition is mounted late in the boot,
the copy doesn't have to wait for it. Once the boot partition is ready the copy
is checked against it, and refreshed (with a quick restart) if you changed
anything - a new version or an edited settings.txt.
"""
import os
import sys

# Started with "python3 -S" (skips scanning for add-on packages at start-up,
# which is slow on a Pi Zero): add the system package folders ourselves.
if sys.flags.no_site:
    _v = f"python{sys.version_info.major}.{sys.version_info.minor}"
    for _p in (f"/usr/local/lib/{_v}/dist-packages", "/usr/lib/python3/dist-packages",
               f"/usr/lib/{_v}/dist-packages"):
        if os.path.isdir(_p) and _p not in sys.path:
            sys.path.append(_p)

import logging  # noqa: E402
import time  # noqa: E402
from pathlib import Path  # noqa: E402

# Several threads share the Pi Zero's single core (screen sender, camera,
# background saving/developing). Python lets one run at a time; switching every
# 1 ms instead of 5 ms keeps the screen sender and camera from waiting so long.
sys.setswitchinterval(0.001)

RUN_DIR = Path(__file__).resolve().parent        # where this code is running from
SD_DIR = Path("/boot/firmware/zeroichi")           # Zero Ichi's home on the SD card
FROM_COPY = RUN_DIR == Path("/var/lib/zeroichi/app")
DATA_DIR = SD_DIR if FROM_COPY else RUN_DIR      # settings, photos' state, logs
os.environ["ZEROICHI_DIR"] = str(DATA_DIR)         # modules use this to find files
os.environ["ZEROICHI_RUN_DIR"] = str(RUN_DIR)
sys.argv[0] = str(RUN_DIR / "zeroichi.py")         # absolute, so re-launching works
sys.dont_write_bytecode = True                   # never write __pycache__ to the SD card

MARKS = []                                       # (step, seconds since power-on)


def uptime():
    try:
        with open("/proc/uptime") as f:
            return float(f.read().split()[0])
    except OSError:
        return -1.0


def mark(step):
    MARKS.append((step, uptime()))


mark("python")

# --- Zero Ichi's own modules, pre-compiled into /var/cache ----------------------
OWN_MODULES = ("config", "gpio", "lcd", "offline", "diag", "bootopt", "camera",
               "app", "filters", "movie", "copysync", "camd")    # (camd: camd_start.py)
CACHE_DIR = Path("/var/cache/zeroichi/lib")


def _cache_is_fresh(src, pyc):
    """Same test Python itself uses: the .pyc remembers the source's mtime+size."""
    try:
        head = pyc.read_bytes()[:16]
        st = src.stat()
        return (len(head) == 16
                and int.from_bytes(head[8:12], "little") == int(st.st_mtime) & 0xFFFFFFFF
                and int.from_bytes(head[12:16], "little") == st.st_size & 0xFFFFFFFF)
    except OSError:
        return False


def prepare_module_cache(only=None):
    """Compile Zero Ichi's modules into the cache (the ones that changed). With
    `only`: just those - the others, if out of date, are dropped from the
    cache (they load from source, and the Python Zero Ichi compiles them when it
    next starts) - an update stays quick (v50: compiling app.py alone took
    the Pi Zero over a second, and the C program never uses it)."""
    try:
        CACHE_DIR.mkdir(parents=True, exist_ok=True)
    except OSError:
        return                                  # read-only? just run from source
    import importlib.util
    import py_compile
    magic = importlib.util.MAGIC_NUMBER
    for name in OWN_MODULES:
        src, pyc = RUN_DIR / f"{name}.py", CACHE_DIR / f"{name}.pyc"
        if not src.exists():
            continue
        try:
            if pyc.read_bytes()[:4] == magic and _cache_is_fresh(src, pyc):
                continue
        except OSError:
            pass
        if only is not None and name not in only:
            pyc.unlink(missing_ok=True)         # (out of date: never used stale)
            continue
        try:
            py_compile.compile(str(src), cfile=str(pyc), dfile=str(src), doraise=True)
        except Exception:
            pyc.unlink(missing_ok=True)         # fall back to the .py (shows the error)
    sys.path.insert(0, str(CACHE_DIR))


# (an update - see maintenance - needs only these: nothing else compiled then)
MAINTENANCE_MODULES = ("config", "copysync", "bootopt", "camd")
prepare_module_cache(MAINTENANCE_MODULES if "--maintenance" in sys.argv else None)
sys.path.insert(1, str(RUN_DIR))

# --- logging: to zeroichi-log.txt on the SD card ---------------------------------
# (When running from the copy the SD card may not be mounted yet: keep the first
#  lines in memory and write them out once it is.)
log = logging.getLogger("zeroichi")
_root = logging.getLogger()
_root.setLevel(logging.INFO)
_fmt = logging.Formatter("%(asctime)s %(levelname)s %(message)s")
_console = logging.StreamHandler()
_console.setFormatter(_fmt)
_root.addHandler(_console)
class _Buffer(logging.Handler):
    """Keeps log lines in memory until the SD card is ready (logging.handlers
    has one, but it is slow to load on a Pi Zero)."""
    def __init__(self):
        super().__init__()
        self.records = []

    def emit(self, record):
        if len(self.records) < 5000:
            self.records.append(record)


_buffer = _Buffer()
_root.addHandler(_buffer)
sys.excepthook = lambda t, v, tb: logging.critical("Zero Ichi crashed", exc_info=(t, v, tb))


def start_file_log():
    # (the C Zero Ichi handed over to this one: keep its lines, they say why)
    cont = os.path.exists("/run/zeroichi/log-continues")
    new = DATA_DIR / "zeroichi-log.txt"
    try:
        fh = logging.FileHandler(new, mode="a" if cont else "w")
    except OSError:
        return
    if cont:
        try:
            os.unlink("/run/zeroichi/log-continues")
        except OSError:
            pass
    fh.setFormatter(_fmt)
    _root.removeHandler(_buffer)
    for record in _buffer.records:
        fh.handle(record)
    _buffer.records.clear()
    _root.addHandler(fh)


if not FROM_COPY:
    start_file_log()

# lgpio creates helper files in the current directory, which fails on the SD
# card's FAT partition. Use /run (ready very early in the boot).
for _wd in ("/run/zeroichi", "/tmp"):
    try:
        os.makedirs(_wd, exist_ok=True)
        os.chdir(_wd)
        os.environ.setdefault("LG_WD", _wd)
        break
    except OSError:
        continue

import config  # noqa: E402



def maintenance():
    """For the fast start, after Zero Ichi's C program found changed files on the
    SD card: refresh the copy and the fast start's own files, then end (the
    fast start starts Zero Ichi again). Nothing on the screen; notes go to the
    end of zeroichi-log.txt."""
    try:
        fh = logging.FileHandler(SD_DIR / "zeroichi-log.txt", mode="a")
        fh.setFormatter(_fmt)
        _root.addHandler(fh)
    except OSError:
        pass
    _root.removeHandler(_buffer)
    import copysync
    try:                                         # (older versions kept the time here: the
        Path("/var/lib/zeroichi/clock").unlink()   #  card's own files keep it now)
    except OSError:
        pass
    try:
        updated = copysync.sync()
        log.info("Copy refreshed from the SD card (%s)", ", ".join(updated[:8]) or "nothing changed")
    except OSError as e:
        log.warning("Could not refresh the copy: %s", e)
    # (the module cache was made from the old copy when this started: made
    #  again from the new one - else the new version's start-up settings below
    #  would only be used at the update after it. Only what is used now and
    #  by the C program's camera helper: the rest when the Python Zero Ichi runs)
    prepare_module_cache(only=MAINTENANCE_MODULES)
    try:
        import bootopt
        bootopt.radios_off()                     # (0.1.0: never any Wi-Fi or Bluetooth)
        s = config.settings(SD_DIR / "settings.txt")
        if s["optimize_boot"]:
            bootopt.sync_fast_start(s["fast_start"], copysync.COPY_DIR)
    except Exception:
        log.exception("Fast start check failed")
    logging.shutdown()
    os.sync()
    sys.exit(0)


if "--maintenance" in sys.argv:
    maintenance()

# settings: from the copy (kept in sync) or straight from the SD card
SETTINGS = config.settings(RUN_DIR / "settings.txt")
log.info("Zero Ichi v%s starting %.1f s after power-on (Python %s) - display: %s%s",
         config.VERSION, uptime(), sys.version.split()[0], SETTINGS["display"],
         " - fast start from copy" if FROM_COPY else "")


def load_state():
    """Saved menu settings (screen flip, rotation) for the splash and camera -
    from the SD card, where they are kept (not mounted yet: settings.txt's)."""
    import json
    try:
        st = json.loads((DATA_DIR / "state.json").read_text())
        if isinstance(st, dict):
            if st.get("screen"):
                st["screen"] = config.display_canon(st["screen"])
            return st
    except (OSError, ValueError):
        pass
    return {}


STATE = load_state()

LCD = None
_bar = {"done": 0}


def choose_screen():
    """The screen type: the one picked in the menu (saved), else settings.txt.
    Holding KEY2 while powering on switches between 320x240 and 240x240 - handy
    when the card goes into the other Zero Ichi and the picture isn't readable."""
    base = SETTINGS["display"]
    serial = config.pi_serial()
    mine = (STATE.get("screens") or {}).get(serial) if serial else None
    if mine:
        STATE["screen"] = config.display_canon(mine)
    profile = STATE.get("screen") if STATE.get("screen") in config.DISPLAYS else base
    try:
        import gpio
        if gpio.held_down(SETTINGS.get("pin_key2") or 20):
            p320 = base if base != "1.3inch-hat" else "2.8inch-hat"
            profile = "1.3inch-hat" if profile != "1.3inch-hat" else p320
            STATE["screen"] = profile
            if serial:
                STATE["screens"] = dict(STATE.get("screens") or {}, **{serial: profile})
            STATE["_saved"] = time.time()
            import json
            try:                                          # (on the SD card only)
                (DATA_DIR / "state.json").write_text(json.dumps(STATE, indent=1))
            except OSError:
                pass
            os.sync()
            log.info("KEY2 held at start-up: screen switched to %s", profile)
    except Exception:
        log.exception("Screen switch check failed")
    return profile


def bigger_spi_buffer():
    """The one-time setup makes the SPI driver load with 64 KB writes (instead of
    4 KB) from the next boot on. Until then, reload it now - before the screen
    is opened - so every frame goes in 4 writes instead of 57."""
    try:
        with open("/sys/module/spidev/parameters/bufsiz") as f:
            if int(f.read().strip()) >= 65536:
                return
        if os.geteuid() != 0:
            return
        import subprocess
        subprocess.run(["modprobe", "-r", "spidev"], capture_output=True, timeout=10)
        r = subprocess.run(["modprobe", "spidev", "bufsiz=65536"], capture_output=True, timeout=10)
        if r.returncode != 0:                      # make sure it's back, whatever happens
            subprocess.run(["modprobe", "spidev"], capture_output=True, timeout=10)
        log.info("SPI driver reloaded with 64 KB writes")
    except Exception as e:
        log.info("SPI buffer unchanged (%s)", e)


HELPER_DIR = Path("/var/lib/zeroichi")             # zeroichi-lcd's settings + picture


def early_splash():
    """Show the pre-rendered splash image before loading the heavy libraries.
    With the fast start, zeroichi-lcd has usually shown it already (seconds
    earlier): then the screen is set up again without a reset, so the picture
    stays on. The settings and picture zeroichi-lcd uses next time are kept up
    to date here."""
    global LCD
    try:
        from lcd import ST7789
        flip = bool(STATE.get("screen_flip", SETTINGS["rotate"] == 180))
        conf_path = HELPER_DIR / "lcd.conf"
        try:
            old_conf = conf_path.read_text()
        except OSError:
            old_conf = None
        shown = os.path.exists("/run/zeroichi/splash-ok")
        LCD = ST7789.from_settings(dict(SETTINGS, display=SCREEN), rotate180=flip, clear=False,
                                   reset=not (shown and old_conf is not None))
        if shown and LCD.helper_config() != old_conf:     # (other screen: set up properly)
            LCD._init_panel(reset=True)
        name = "splash.rgb565" if LCD.WIDTH == 240 else f"splash-{LCD.WIDTH}x{LCD.HEIGHT}.rgb565"
        try:
            data = (RUN_DIR / name).read_bytes()
            LCD.show_bytes(data)
        except OSError:
            data = None
            LCD.clear((242, 234, 216))          # splash file missing: plain cream (Zero Ichi's)
        try:                                    # for zeroichi-lcd at the next start
            if os.geteuid() == 0 and HELPER_DIR.is_dir():
                if LCD.helper_config() != old_conf:
                    conf_path.write_text(LCD.helper_config())
                serial = config.pi_serial()                 # (this Pi's own, see zeroichi-lcd.c)
                own = HELPER_DIR / f"lcd-{serial}.conf"
                if serial and (not own.exists() or own.read_text() != LCD.helper_config()):
                    own.write_text(LCD.helper_config())
                pic = HELPER_DIR / "splash.rgb565"
                if data and (not pic.exists() or pic.read_bytes() != data):
                    pic.write_bytes(data)
        except OSError:
            pass
    except Exception:
        log.exception("Early splash failed")


def set_thread_priority(nice):
    """Linux priorities are per thread: lower this thread's priority while it
    loads things that aren't urgent, so the camera and the system's device setup
    (which the camera waits for) get the processor first."""
    try:
        os.setpriority(os.PRIO_PROCESS, __import__("threading").get_native_id(), nice)
    except (OSError, AttributeError):
        pass


def start_camera():
    """Start loading the camera in the background as early as possible, with
    the stream size the app will want (so it doesn't have to set it up twice)."""
    from camera import CameraLoader, flips_for, stream_plan
    from lcd import PROFILES
    p = PROFILES.get(SCREEN, PROFILES["1.3inch-hat"])
    rot = STATE.get("cam_rotate") if STATE.get("cam_rotate") in (0, 90, 180, 270) else 0
    h, v, _ = flips_for(rot, bool(STATE.get("mirror")))
    main, lores, photo = stream_plan(STATE, p["width"], p["height"])
    return CameraLoader(fake=SETTINGS["no_camera"] or "--no-camera" in sys.argv,
                        swap_rb=SETTINGS["swap_rb"], hflip=h, vflip=v,
                        preview_size=main, lores=lores, main_yuv=photo,
                        video_yuv=STATE.get("mode") == "VIDEO")


def progress(fraction):
    """Grow a thin bar along the bottom of the splash screen."""
    if LCD is None:
        return
    try:
        w = max(1, min(LCD.WIDTH, int(LCD.WIDTH * fraction)))
        if w <= _bar["done"]:
            return
        x0 = _bar["done"]
        _bar["done"] = w
        LCD.show_region(x0, LCD.HEIGHT - 6, w - 1, LCD.HEIGHT - 3,
                        b"\x18\xc3" * ((w - x0) * 4))       # a black bar (Zero Ichi's black)
    except Exception:
        pass


def wait_for_sd_card(timeout=90):
    """Running from the copy: wait until the SD card's boot partition is mounted."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if os.path.ismount("/boot/firmware") and SD_DIR.is_dir():
            return True
        time.sleep(0.1)
    return False


def refresh_copy():
    """If anything changed on the SD card, refresh the copy and restart it."""
    import copysync
    try:
        changed = copysync.changed()
        if changed:
            log.info("Files changed on the SD card (%s) - updating and restarting",
                     ", ".join(changed[:6]))
            copysync.sync()
            if LCD is not None:
                LCD.release()
            logging.shutdown()
            os.execv(sys.executable, [sys.executable, str(RUN_DIR / "zeroichi.py")] + sys.argv[1:])
        copysync.sync()                          # (tidies the copy)
    except OSError as e:
        log.warning("Could not check the copy: %s", e)


if __name__ == "__main__":
    SCREEN = choose_screen()
    bigger_spi_buffer()
    early_splash()                               # splash first (takes ~1 s) ...
    mark("splash")
    LOADER = start_camera()                      # ... then the camera, the slowest part
    progress(0.1)
    set_thread_priority(5)                       # until the camera is ready

    import offline
    if not offline.deps_ok():                    # numpy / Pillow / picamera2 missing
        if os.environ.get("ZEROICHI_FASTINIT") == "1":
            # installing needs the normal start-up: ask the fast start for it
            log.info("Python packages missing - restarting with the normal start-up")
            sys.exit(65)
        if FROM_COPY:
            wait_for_sd_card()
            start_file_log()
        offline.ensure_packages(lcd=LCD)        # installs from the SD card

    progress(0.2)

    import numpy  # noqa: F401,E402
    mark("numpy")
    progress(0.4)
    from PIL import Image, ImageDraw, ImageFont, ImageOps, ImageSequence  # noqa: F401,E402
    mark("pillow")
    progress(0.5)
    import app  # noqa: E402
    mark("app")
    progress(0.6)

    if FROM_COPY:
        if wait_for_sd_card():
            mark("sd card")
            start_file_log()
            refresh_copy()                       # restarts if you changed files
        else:
            log.error("SD card boot partition not found - running from the copy only")
    if SETTINGS["diagnostics"]:
        try:
            import diag
            diag.write_diagnostics()
        except Exception:
            log.exception("Diagnostics failed")
    app.main(SETTINGS, lcd=LCD, started=uptime, progress=progress,
             camera_loader=LOADER, marks=MARKS)
