"""
Keeps a copy of Zero Ichi on the Pi's main (ext4) partition.

Zero Ichi's files live on the SD card's boot partition (so you can edit them from
a computer), but that partition is only mounted late in the boot. Running from
a copy in /var/lib/zeroichi/app lets Zero Ichi start ~10 s earlier. At each start,
once the boot partition is ready, the copy is compared with the SD card and
refreshed if you changed anything (new version, edited settings.txt).
"""
import os
import shutil
from pathlib import Path

SD_DIR = Path("/boot/firmware/zeroichi")
COPY_DIR = Path("/var/lib/zeroichi/app")
CODE_SUFFIXES = (".py", ".rgb565")
PROGRAMS = ("zeroichi-c",)                   # Zero Ichi itself (the C program): runnable
RESTART_FILES = ("settings.txt",) + PROGRAMS   # a change here also needs a restart
OLD_FILES = ("state.json",)                # (older versions kept a copy of your settings
                                           # here: they are on the SD card only now)
# (v55) files of older versions the kit no longer has: an update copies the
# new files over the old ones, so these stay on the card - removed there too
OBSOLETE = ("whitebalance.py",)


def _files(src):
    out = []
    try:
        for p in src.iterdir():
            if p.name.startswith((".", "toycam")):   # (v53: Zero Ichi's old name's files)
                continue
            if p.suffix in CODE_SUFFIXES or p.name in RESTART_FILES:
                out.append(p.name)
    except OSError:
        pass
    return sorted(out)


def _same(a, b):
    try:
        sa, sb = a.stat(), b.stat()
    except OSError:
        return False
    if sa.st_size != sb.st_size:
        return False
    if abs(sa.st_mtime - sb.st_mtime) <= 2:     # FAT keeps times to 2 s
        return True
    return a.read_bytes() == b.read_bytes()


def changed(src=SD_DIR, dst=COPY_DIR):
    """Names of code/settings files that differ between the SD card and the copy."""
    return [n for n in _files(src) if not _same(src / n, dst / n)]


def sync(src=SD_DIR, dst=COPY_DIR):
    """Copy code + settings from the SD card. Returns the list of files that
    were updated. (Your own settings - state.json, presets - stay on the SD
    card only.)"""
    dst.mkdir(parents=True, exist_ok=True)
    for name in OBSOLETE:                       # (the card read-only now? then the next time)
        try:
            (src / name).unlink()
        except OSError:
            pass
    updated = []
    for name in _files(src):
        if not _same(src / name, dst / name):
            tmp = dst / (name + ".tmp")
            shutil.copy2(src / name, tmp)
            if name in PROGRAMS:
                os.chmod(tmp, 0o755)
            os.replace(tmp, dst / name)
            updated.append(name)
    wrote = bool(updated)
    for name in OLD_FILES:
        try:
            if (dst / name).exists():
                (dst / name).unlink()
                wrote = True
        except OSError:
            pass
    for stale in list(dst.glob("*.py")) + [dst / n for n in PROGRAMS]:   # removed from the SD card
        if stale.exists() and not (src / stale.name).exists():
            stale.unlink(missing_ok=True)
            wrote = True
    for name in PROGRAMS:                       # (must be runnable, however it got there)
        p = dst / name
        try:
            if p.exists() and not os.access(p, os.X_OK):
                os.chmod(p, 0o755)
        except OSError:
            pass
    if wrote:
        os.sync()
    return updated
