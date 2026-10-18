"""System report written to the SD card (only when diagnostics = yes in settings.txt)."""
import logging
import os
from pathlib import Path

# Where Zero Ichi lives on the SD card. (Set by zeroichi.py: these modules may be
# loaded from the compiled cache in /var/cache, so __file__ points there.)
APP_DIR = Path(os.environ.get("ZEROICHI_DIR") or Path(__file__).resolve().parent)


def write_diagnostics():
    """Once per boot, write a system report next to this script (readable on a PC)."""
    import os
    import subprocess
    marker = Path("/run/zeroichi-diag-done")
    if marker.exists():
        return
    out = []

    def run(title, cmd, timeout=20):
        try:
            r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
            txt = (r.stdout + r.stderr).strip()
        except Exception as e:
            txt = f"(failed: {e})"
        out.append(f"===== {title} =====\n{txt}\n")

    run("date / kernel", "date; uname -a; cat /proc/device-tree/model; echo")
    run("OS", "grep PRETTY_NAME /etc/os-release")
    run("power (0x0 = OK, anything else = weak power supply)", "vcgencmd get_throttled")
    run("SPI device (needs /dev/spidev0.0)", "ls -l /dev/spidev* /dev/gpiomem* 2>&1")
    run("config.txt SPI / camera lines", "grep -n -i -E 'spi|camera|dtoverlay' /boot/firmware/config.txt")
    for mod in ("numpy", "PIL", "gpiozero", "lgpio", "spidev", "libcamera", "picamera2"):
        run(f"python import {mod}",
            f"python3 -c \"import {mod}; print('OK', getattr({mod}, '__version__', ''))\"")
    run("cameras", "rpicam-hello --list-cameras || libcamera-hello --list-cameras", timeout=40)
    run("zeroichi service", "systemctl is-enabled zeroichi; systemctl status zeroichi --no-pager -n 0")
    run("zeroichi log (previous boot)", "journalctl -u zeroichi -b -1 -n 40 --no-pager")
    run("cloud-init", "cloud-init status --long")
    run("boot time", "systemd-analyze; systemd-analyze blame --no-pager | head -25")
    run("zeroichi startup timing", "grep -E \"power-on|Ready\" " + str(APP_DIR / "zeroichi-log.txt"))
    try:
        path = APP_DIR / "zeroichi-diagnostics.txt"
        with open(path, "w") as f:
            f.write("\n".join(out))
            f.flush()
            os.fsync(f.fileno())
        marker.touch()
        logging.info("Wrote %s", path)
    except OSError as e:
        logging.warning("Could not write diagnostics: %s", e)
