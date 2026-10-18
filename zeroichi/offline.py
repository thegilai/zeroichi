"""
Offline package installer for Zero Ichi.

The Pi Zero v1.3 has no network, so missing packages are fetched by a PC:

  1. The Pi writes offline/download-list.txt (what it needs) and powers off.
  2. On the PC, GET-PACKAGES.bat downloads everything onto the SD card,
     laid out exactly like the real Raspberry Pi OS package server.
  3. Back on the Pi, apt uses the SD card as its package server. It checks
     every signature and checksum, just like a normal online install.

This file uses only the Python standard library (numpy/Pillow may be missing).
"""
import hashlib
import importlib.util
import logging
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.parse
from email.utils import parsedate_to_datetime
from pathlib import Path

BOOT = Path(os.environ.get("ZEROICHI_BOOT", "/boot/firmware"))
OFF = BOOT / "offline"
MIRROR = OFF / "mirror"
LIST = OFF / "download-list.txt"
STATUS = OFF / "STATUS.txt"
WORK = Path(os.environ.get("ZEROICHI_OFFLINE_WORK", "/var/lib/zeroichi-offline"))

PACKAGES = os.environ.get("ZEROICHI_PACKAGES",
                          "python3-numpy python3-pil python3-picamera2").split()
MODULES = os.environ.get("ZEROICHI_MODULES", "numpy PIL picamera2").split()

log = logging.getLogger("zeroichi.offline")


# ---------------------------------------------------------------- screen
_FONT = {
    "A": ".###.#...##...#######...##...##...#", "B": "####.#...##...#####.#...##...#####.",
    "C": ".###.#...##....#....#....#...#.###.", "D": "####.#...##...##...##...##...#####.",
    "E": "######....#....####.#....#....#####", "F": "######....#....####.#....#....#....",
    "G": ".###.#...##....#.####...##...#.####", "H": "#...##...##...#######...##...##...#",
    "I": ".###...#....#....#....#....#...###.", "J": "..###...#....#....#....#.#..#..##..",
    "K": "#...##..#.#.#..##...#.#..#..#.#...#", "L": "#....#....#....#....#....#....#####",
    "M": "#...###.###.#.##.#.##...##...##...#", "N": "#...##...###..##.#.##..###...##...#",
    "O": ".###.#...##...##...##...##...#.###.", "P": "####.#...##...#####.#....#....#....",
    "Q": ".###.#...##...##...##.#.##..#..##.#", "R": "####.#...##...#####.#.#..#..#.#...#",
    "S": ".#####....#.....###.....#....#####.", "T": "#####..#....#....#....#....#....#..",
    "U": "#...##...##...##...##...##...#.###.", "V": "#...##...##...##...##...#.#.#...#..",
    "W": "#...##...##...##.#.##.#.##.#.#.#.#.", "X": "#...##...#.#.#...#...#.#.#...##...#",
    "Y": "#...##...#.#.#...#....#....#....#..", "Z": "#####....#...#...#...#...#....#####",
    "0": ".###.#...##..###.#.###..##...#.###.", "1": "..#...##....#....#....#....#...###.",
    "2": ".###.#...#....#...#...#...#...#####", "3": "#####...#...#.....#.....##...#.###.",
    "4": "...#...##..#.#.#..#.#####...#....#.", "5": "######....####.....#....##...#.###.",
    "6": "..##..#...#....####.#...##...#.###.", "7": "#####....#...#...#...#....#....#...",
    "8": ".###.#...##...#.###.#...##...#.###.", "9": ".###.#...##...#.####....#...#..##..",
    ".": "..........................##...##..", ",": ".....................##....#...#...",
    ":": ".....##...##.........##...##.......", "-": "...............#####...............",
    "!": "..#....#....#....#....#.........#..", "?": ".###.#...#....#...#...#.........#..",
    "/": ".........#...#...#...#...#.........", "(": "...#...#...#....#....#.....#.....#.",
    ")": ".#.....#.....#....#....#...#...#...", "'": "..#....#...#.......................",
    "%": "##...##..#...#...#...#...#..##...##", " ": "." * 35,
}


class Screen:
    """Text-only status screens without numpy/Pillow (5x7 bitmap font)."""
    W = H = 240

    def __init__(self, lcd=None):
        self.lcd, self.own = lcd, lcd is None
        if lcd is None:
            try:
                import config
                from lcd import ST7789
                self.lcd = ST7789.from_settings(config.settings())
            except Exception as e:
                log.warning("LCD not available for status screen: %s", e)
        if self.lcd is not None:
            self.W, self.H = self.lcd.WIDTH, self.lcd.HEIGHT

    @staticmethod
    def _rgb565(c):
        r, g, b = c
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        return bytes((v >> 8, v & 0xFF))

    @staticmethod
    def _wrap(text, width):
        words, lines, cur = text.upper().split(), [], ""
        for w in words:
            if cur and len(cur) + 1 + len(w) > width:
                lines.append(cur)
                cur = w
            else:
                cur = f"{cur} {w}" if cur else w
        if cur:
            lines.append(cur)
        return lines

    def show(self, title, body="", bg=(20, 20, 40), title_color=(255, 220, 80)):
        if self.lcd is None:
            return
        buf = bytearray(self._rgb565(bg) * (self.W * self.H))
        blocks = [(self._wrap(title, self.W // 18), 3, title_color),
                  (self._wrap(body, self.W // 12), 2, (235, 235, 235))]
        rows = sum(len(l) * (8 * s + 4) for l, s, _ in blocks) + 10
        y = max(4, (self.H - rows) // 2)
        for lines, s, color in blocks:
            px = self._rgb565(color) * s
            for line in lines:
                x = (self.W - len(line) * 6 * s) // 2
                for ch in line:
                    glyph = _FONT.get(ch, _FONT["?"])
                    for gy in range(7):
                        for gx in range(5):
                            if glyph[gy * 5 + gx] == "#":
                                for dy in range(s):
                                    i = ((y + gy * s + dy) * self.W + x + gx * s) * 2
                                    buf[i:i + 2 * s] = px
                    x += 6 * s
                y += 8 * s + 4
            y += 10
        try:
            self.lcd.show_bytes(bytes(buf))
        except Exception as e:
            log.warning("LCD write failed: %s", e)

    def close(self):
        if self.lcd and self.own:
            self.lcd.release()


# ---------------------------------------------------------------- apt sources
def _parse_list(text):
    repos = []
    for line in text.splitlines():
        line = line.split("#", 1)[0].strip()
        if not line.startswith("deb "):
            continue
        opts = {}
        m = re.match(r"deb\s+\[([^\]]*)\]\s+(.*)", line)
        if m:
            for kv in m.group(1).split():
                if "=" in kv:
                    k, v = kv.split("=", 1)
                    opts[k.lower()] = v
            rest = m.group(2).split()
        else:
            rest = line.split()[1:]
        if len(rest) >= 3:
            repos.append({"uri": rest[0], "suites": [rest[1]], "components": rest[2:],
                          "signed_by": opts.get("signed-by", "")})
    return repos


def _parse_deb822(text):
    repos = []
    for para in re.split(r"\n\s*\n", text):
        fields, key = {}, None
        for raw in para.splitlines():
            if raw.startswith("#"):
                continue
            if raw[:1] in (" ", "\t") and key:
                fields[key] += "\n" + raw
            elif ":" in raw:
                key, val = raw.split(":", 1)
                key = key.strip().lower()
                fields[key] = val.strip()
        if "deb" not in fields.get("types", "").split():
            continue
        if fields.get("enabled", "yes").lower() == "no":
            continue
        for uri in fields.get("uris", "").split():
            repos.append({"uri": uri, "suites": fields.get("suites", "").split(),
                          "components": fields.get("components", "").split(),
                          "signed_by": fields.get("signed-by", "")})
    return repos


def read_sources():
    root = Path(os.environ.get("ZEROICHI_APT_ETC", "/etc/apt"))
    files = []
    if (root / "sources.list").exists():
        files.append(root / "sources.list")
    d = root / "sources.list.d"
    if d.is_dir():
        files += sorted(d.glob("*.list")) + sorted(d.glob("*.sources"))
    repos, seen = [], set()
    for f in files:
        text = f.read_text(errors="replace")
        for r in (_parse_deb822(text) if f.suffix == ".sources" else _parse_list(text)):
            if not r["uri"].startswith(("http://", "https://")):
                continue
            key = (r["uri"].rstrip("/"), tuple(r["suites"]))
            if key not in seen and r["components"]:
                seen.add(key)
                repos.append(r)
    return repos


def mirror_name(uri):
    u = urllib.parse.urlparse(uri)
    path = re.sub(r"[^A-Za-z0-9._/-]", "_", u.path.strip("/"))
    return f"{u.hostname}/{path}".rstrip("/")


def arch():
    return subprocess.run(["dpkg", "--print-architecture"], capture_output=True,
                          text=True).stdout.strip() or "armhf"


# ---------------------------------------------------------------- apt wrapper
def _apt_opts():
    return [
        "-o", f"Dir::Etc::SourceList={WORK / 'empty.list'}",
        "-o", f"Dir::Etc::SourceParts={WORK / 'sources.d'}",
        "-o", f"Dir::State::Lists={WORK / 'lists'}",
        "-o", f"Dir::Cache={WORK / 'cache'}",
        "-o", "Acquire::Languages=none",
        "-o", "Acquire::By-Hash=no",
        "-o", "Acquire::Check-Date=false",
        "-o", "Acquire::Check-Valid-Until=false",
        "-o", "APT::Get::List-Cleanup=false",
    ]


def apt_get(*args, logfile=None):
    env = dict(os.environ, DEBIAN_FRONTEND="noninteractive", LC_ALL="C")
    cmd = ["apt-get", *_apt_opts(), *args]
    log.info("Running: apt-get %s", " ".join(args))
    if logfile:
        with open(logfile, "a") as f:
            f.write(f"\n$ apt-get {' '.join(args)}\n")
            f.flush()
            r = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env)
        return r.returncode, ""
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def write_mirror_sources(repos, a):
    (WORK / "sources.d").mkdir(parents=True, exist_ok=True)
    (WORK / "lists" / "partial").mkdir(parents=True, exist_ok=True)
    (WORK / "cache" / "archives" / "partial").mkdir(parents=True, exist_ok=True)
    (WORK / "empty.list").write_text("")
    for f in (WORK / "sources.d").glob("*"):
        f.unlink()
    out = []
    for r in repos:
        local = MIRROR / mirror_name(r["uri"])
        para = [f"Types: deb", f"URIs: file:{local}", f"Suites: {' '.join(r['suites'])}",
                f"Components: {' '.join(r['components'])}", f"Architectures: {a}"]
        if r["signed_by"]:
            sb = r["signed_by"]
            para.append(f"Signed-By: {sb}" if "\n" not in sb else f"Signed-By:{sb if sb.startswith(chr(10)) else ' ' + sb}")
        out.append("\n".join(para))
    (WORK / "sources.d" / "zeroichi-offline.sources").write_text("\n\n".join(out) + "\n")


# ---------------------------------------------------------------- steps
def index_downloads(repos, a):
    items = []
    for r in repos:
        base, rel = r["uri"].rstrip("/"), f"mirror/{mirror_name(r['uri'])}"
        for suite in r["suites"]:
            items.append(("REQ", f"{base}/dists/{suite}/InRelease", f"{rel}/dists/{suite}/InRelease"))
            for comp in r["components"]:
                for ar in (a, "all"):
                    for ext in ("xz", "gz"):
                        p = f"dists/{suite}/{comp}/binary-{ar}/Packages.{ext}"
                        items.append(("OPT", f"{base}/{p}", f"{rel}/{p}"))
    return items


def write_list(items, note):
    OFF.mkdir(parents=True, exist_ok=True)
    with open(LIST, "w", newline="\r\n") as f:
        f.write(f"# {note}\n# REQ/OPT <tab> URL <tab> path inside the offline folder\n")
        for kind, url, rel in items:
            f.write(f"{kind}\t{url}\t{rel}\n")
        f.flush()
        os.fsync(f.fileno())


def set_status(text):
    OFF.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(STATUS, "w", newline="\r\n") as f:
        f.write(f"[{stamp}]\n{text}\n")
        f.flush()
        os.fsync(f.fileno())
    log.info("STATUS: %s", text.replace("\n", " | "))


def fix_clock(repos):
    """No RTC + no network = the clock can be weeks behind, which makes signature
    checks fail ('signed in the future'). Move it forward to the index date."""
    newest = None
    for r in repos:
        for suite in r["suites"]:
            p = MIRROR / mirror_name(r["uri"]) / "dists" / suite / "InRelease"
            try:
                m = re.search(r"^Date:\s*(.+)$", p.read_text(errors="replace"), re.M)
                if m:
                    d = parsedate_to_datetime(m.group(1).strip()).timestamp()
                    newest = d if newest is None else max(newest, d)
            except (OSError, ValueError, TypeError):
                pass
    if newest and newest + 60 > time.time() and os.environ.get("ZEROICHI_NO_CLOCK") != "1":
        log.info("Setting clock forward to package index date")
        subprocess.run(["date", "-s", f"@{int(newest + 120)}"], capture_output=True)


def parse_print_uris(text, repos):
    """Turn apt's file: URIs back into real download URLs."""
    prefixes = [(f"file:{MIRROR / mirror_name(r['uri'])}/", r["uri"].rstrip("/") + "/",
                 f"mirror/{mirror_name(r['uri'])}/") for r in repos]
    items, total = [], 0
    for line in text.splitlines():
        m = re.match(r"'([^']+)'\s+(\S+)\s+(\d+)\s+(\S+)", line)
        if not m:
            continue
        uri, size, digest = urllib.parse.unquote(m.group(1)), int(m.group(3)), m.group(4)
        uri = uri.replace("file:///", "file:/", 1)
        for local, remote, rel in prefixes:
            if uri.startswith(local):
                tail = uri[len(local):]
                items.append(("REQ", remote + tail, rel + tail, size, digest))
                total += size
                break
    return items, total


def file_ok(path, size, digest):
    """True if the file exists with the right size and checksum. Bad files are
    deleted so the PC downloads them again."""
    if not path.is_file():
        return False
    algo, _, want = digest.partition(":")
    algo = {"SHA256": "sha256", "SHA512": "sha512", "SHA1": "sha1", "MD5Sum": "md5"}.get(algo)
    good = path.stat().st_size == size
    if good and algo and want:
        h = hashlib.new(algo)
        with open(path, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        good = h.hexdigest().lower() == want.lower()
    if not good:
        log.warning("Bad download, will fetch again: %s", path.name)
        path.unlink()
    return good


def need_pc(screen, items, note, mb=None):
    write_list([i[:3] for i in items], note)
    size = f" (about {mb:.0f} MB)" if mb else ""
    set_status(f"WAITING FOR YOUR PC{size}.\n{note}\n"
               "Put the SD card in your PC and double-click GET-PACKAGES.bat,\n"
               "then put the card back in the Pi and power it on.")
    screen.show("PC STEP NEEDED",
                f"Put the SD card in your PC and run GET-PACKAGES{size}. "
                "Wait until the green LED is off before unplugging.")
    os.sync()
    poweroff()


def poweroff():
    if os.environ.get("ZEROICHI_NO_POWEROFF") == "1":
        raise SystemExit(3)
    time.sleep(3)
    subprocess.run(["systemctl", "poweroff"])
    while True:
        time.sleep(60)


def deps_ok():
    return all(importlib.util.find_spec(m) is not None for m in MODULES)


def ensure_packages(lcd=None):
    """Return if everything is installed; otherwise drive the offline install.
    Never returns while packages are missing (it powers off or re-launches)."""
    if deps_ok():
        return
    log.info("Missing Python packages - starting offline installer")
    screen = Screen(lcd)
    try:
        _run(screen)
    except SystemExit:
        raise
    except Exception as e:
        log.exception("Offline installer failed")
        set_status(f"ERROR: {e}\nSee zeroichi/zeroichi-log.txt for details.")
        screen.show("SETUP ERROR", "See offline/STATUS.txt on your PC.", bg=(90, 10, 10),
                    title_color=(255, 255, 255))
        os.sync()
        poweroff()


def _run(screen):
    repos = read_sources()
    if not repos:
        raise RuntimeError("no apt sources found")
    a = arch()
    screen.show("PREPARING", "Checking package files. This can take a few minutes.")
    write_mirror_sources(repos, a)
    fix_clock(repos)

    code, out = apt_get("update")
    have_lists = any((WORK / "lists").glob("*_Packages*"))
    if code != 0 or not have_lists or re.search(r"^(E:|Err:)", out, re.M):
        log.info("apt update on SD-card mirror not usable yet:\n%s", out[-2000:])
        if any(MIRROR.rglob("InRelease")):
            set_status("Package index check failed. Details:\n" + out[-1500:])
        need_pc(screen, index_downloads(repos, a),
                "Step 1 of 2: download the package lists.", mb=40)

    code, out = apt_get("install", "--print-uris", "-qq", "-y",
                        "--no-install-recommends", *PACKAGES)
    if code != 0:
        raise RuntimeError("apt could not plan the install:\n" + out[-1500:])
    items, total = parse_print_uris(out, repos)
    screen.show("PREPARING", "Checking downloaded packages...")
    missing = [i for i in items if not file_ok(OFF / i[2], i[3], i[4])]
    if missing:
        mb = sum(i[3] for i in missing) / 1e6
        need_pc(screen, missing, f"Step 2 of 2: download {len(missing)} packages.", mb=mb)

    screen.show("INSTALLING", "This takes 10 to 20 minutes. Please don't unplug!")
    set_status("Installing packages... (do not unplug)")
    logfile = OFF / "install-log.txt"
    code, _ = apt_get("install", "-y", "--no-install-recommends",
                      "-o", "Dpkg::Options::=--force-confdef",
                      "-o", "Dpkg::Options::=--force-confold", *PACKAGES, logfile=logfile)
    os.sync()
    importlib.invalidate_caches()
    if code != 0 or not deps_ok():
        raise RuntimeError("package install failed - see offline/install-log.txt")

    shutil.rmtree(MIRROR, ignore_errors=True)        # free up the SD card space
    try:
        LIST.unlink()
    except FileNotFoundError:
        pass
    set_status("All packages installed. Zero Ichi is ready!")
    os.sync()
    screen.show("ALL DONE!", "Starting Zero Ichi...", bg=(10, 70, 20), title_color=(255, 255, 255))
    time.sleep(2)
    if screen.lcd:
        screen.lcd.release()            # free SPI/GPIO before restarting
    os.execv(sys.executable, [sys.executable] + sys.argv)   # restart with new packages
