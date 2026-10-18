"""
One-time boot speed-up for Zero Ichi on a Pi Zero (runs as root, only on a Pi).

  * turns off cloud-init after first setup (it waits for a network at every boot)
  * turns off services a network-less toy camera never needs
  * swap stays in compressed RAM, without the swap file on the SD card that
    was resized at every boot (~19 s of heavy SD-card writing on a Pi Zero)
  * Zero Ichi starts as soon as the SD card is ready, with high CPU priority
  * firmware: no rainbow splash, no boot delay, full CPU speed while booting,
    and no initramfs (a 14 MB early-boot image the Pi doesn't need - only if a
    safety check confirms the SD card drivers are built into the kernel)
  * quieter kernel start-up messages
  * power-off: no service may hold it up for more than 10 s
  * Zero Ichi runs from a copy on the main partition (doesn't wait for the boot
    partition), and the screen's SPI driver is loaded at the very start
  * a few unused early-boot jobs are switched off (masked)
  * fast start (from v34): the kernel starts Zero Ichi's own small start-up
    (fast-start.sh, installed as /usr/local/sbin/zeroichi-init) instead of the
    normal one - see that file. "fast_start = no" in settings.txt switches
    just this off; KEY1 held at power-on skips it once.

Set  optimize_boot = no  in settings.txt and everything is undone on the next
start. Every change is listed in zeroichi-log.txt.
"""
import logging
import os
import re
import threading
import time
from pathlib import Path

# (json, platform and subprocess are loaded where they are used: an update's
#  quick check - sync_fast_start - rarely needs them, and they take a moment
#  to load on a Pi Zero)

VERSION = 10
RUNNING = threading.Event()          # set while changes are being made

MARK = Path("/var/lib/zeroichi/boot-optimized")
CONFIG_TXT = Path("/boot/firmware/config.txt")
CMDLINE_TXT = Path("/boot/firmware/cmdline.txt")
UNIT = Path("/etc/systemd/system/zeroichi.service")
CLOUD_OFF = Path("/etc/cloud/cloud-init.disabled")
DROPIN = Path("/etc/systemd/system.conf.d/zeroichi-fast-shutdown.conf")
DROPIN_TEXT = "[Manager]\nDefaultTimeoutStopSec=10s\n"
SWAP_DROPIN = Path("/etc/rpi/swap.conf.d/80-zeroichi-zram-only.conf")
SWAP_TEXT = "# Zero Ichi: compressed-RAM swap only, no swap file on the SD card\n" \
            "[Main]\nMechanism=zram\n"
CMDLINE_ADD = ("quiet", "loglevel=3")
MODULES_CONF = Path("/etc/modules-load.d/zeroichi.conf")
MODULES_TEXT = "# Zero Ichi: load the screen and camera drivers at the very start of boot\n" \
               "spi_bcm2835\nspidev\n"
# camera drivers to preload - only ones this Pi actually uses (read from /proc/modules)
CAMERA_MODULES = re.compile(r"^(bcm2835_unicam\w*|unicam\w*|bcm2835_isp|ov\d{4}\w*|"
                            r"imx\d{3}\w*|i2c_mux_pinctrl|i2c_bcm2835)$")
SPIDEV_CONF = Path("/etc/modprobe.d/zeroichi-spidev.conf")
SPIDEV_TEXT = "# Zero Ichi: bigger SPI writes (a screen frame in 4 writes instead of 57)\n" \
              "options spidev bufsiz=65536\n"
COPY_DIR = Path("/var/lib/zeroichi/app")
SD_DIR = Path("/boot/firmware/zeroichi")
# Early-boot jobs a camera never needs (masked = switched off; undone by revert)
MASK = ["systemd-binfmt.service", "sys-fs-fuse-connections.mount",
        "sys-kernel-config.mount", "sys-kernel-debug.mount", "sys-kernel-tracing.mount",
        "console-setup.service"]

FAST_UNIT = """[Unit]
Description=Zero Ichi toy camera
DefaultDependencies=no
After=systemd-remount-fs.service systemd-journald.socket
Conflicts=shutdown.target
Before=shutdown.target

[Service]
WorkingDirectory=/
# Run the copy on the main partition if it's there (starts without waiting for
# the boot partition); otherwise the original on the SD card.
ExecStart=/bin/sh -c 'if [ -f /var/lib/zeroichi/app/zeroichi.py ]; then exec /usr/bin/python3 -S /var/lib/zeroichi/app/zeroichi.py; else exec /usr/bin/python3 -S /boot/firmware/zeroichi/zeroichi.py; fi'
Restart=always
RestartSec=3
TimeoutStopSec=5
# Normal priority on purpose: a higher one slowed down the system's device setup,
# which the camera has to wait for.

[Install]
WantedBy=multi-user.target
"""

NORMAL_UNIT = """[Unit]
Description=Zero Ichi toy camera
RequiresMountsFor=/boot/firmware
After=local-fs.target

[Service]
Environment=PYTHONDONTWRITEBYTECODE=1
WorkingDirectory=/boot/firmware/zeroichi
ExecStart=/usr/bin/python3 /boot/firmware/zeroichi/zeroichi.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
"""

DISABLE = [
    "cloud-init-local.service", "cloud-init-network.service", "cloud-init-main.service",
    "cloud-init.service", "cloud-config.service", "cloud-final.service",
    "NetworkManager-wait-online.service", "systemd-networkd-wait-online.service",
    "ModemManager.service", "bluetooth.service", "hciuart.service",
    "avahi-daemon.service", "avahi-daemon.socket",
    "triggerhappy.service", "triggerhappy.socket",
    "apt-daily.timer", "apt-daily-upgrade.timer", "man-db.timer",
    "e2scrub_all.timer", "e2scrub_reap.service", "dpkg-db-backup.timer",
    # the Pi Zero v1.3 has no network hardware at all:
    "NetworkManager.service", "NetworkManager-dispatcher.service",
    "wpa_supplicant.service", "sshswitch.service",
    # console keyboard layout setup (slow on a Pi Zero; no keyboard on a camera)
    "keyboard-setup.service", "rpi-eeprom-update.service",
]

FW_START, FW_END = "# Zero Ichi fast boot", "# Zero Ichi fast boot end"

# --- fast start: Zero Ichi's own start-up instead of systemd ---------------------
INIT_SRC = "fast-start.sh"                       # in the zeroichi folder
LCD_SRC = "zeroichi-lcd"                           # (its source: src/zeroichi-lcd.c)
LCD_DST = Path("/usr/local/sbin/zeroichi-lcd")
INIT_DST = Path("/usr/local/sbin/zeroichi-init")
INIT_TOKEN = f"init={INIT_DST}"
INIT_MARKER = "zeroichi-init"
MODULES_LIST = Path("/var/lib/zeroichi/modules.list")
NORMAL_FLAG = Path("/var/lib/zeroichi/normal-start")   # next start: the normal start-up
PROC_MODULES = Path("/proc/modules")
# drivers the fast start never needs to load: sound, radio, input, and the HDMI
# graphics driver (the screen is on SPI; the camera doesn't use it)
SKIP_MODULES = re.compile(r"^(snd\w*|bluetooth|btbcm|btintel|hci_uart|brcmfmac\w*|brcmutil|"
                          r"cfg80211|rfkill|joydev|fuse|binfmt_misc|zram|lz4\w*|zstd\w*|ipv6|"
                          r"vc4|v3d|drm\w*|gpu_sched|cec)$")

log = logging.getLogger("zeroichi.bootopt")


# ---------------------------------------------------------------- helpers
def _run(*cmd):
    import subprocess
    # Normal priority on purpose: at the lowest priority the camera preview
    # starved it and it took minutes.
    r = subprocess.run(list(cmd), capture_output=True, text=True)
    return r.returncode == 0


def _unit_states():
    """All unit files and their state, with ONE systemctl call."""
    import subprocess
    r = subprocess.run(["systemctl", "list-unit-files", "--no-legend", "--no-pager"],
                       capture_output=True, text=True)
    states = {}
    for line in r.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            states[parts[0]] = parts[1]
    return states


def _write(path, text):
    tmp = path.with_name(path.name + ".zeroichi-tmp")
    with open(tmp, "w") as f:
        f.write(text)
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)


def _marker():
    import json
    try:
        data = json.loads(MARK.read_text())
        return data if isinstance(data, dict) else None
    except (OSError, ValueError):
        return None


def on_raspberry_pi():
    try:
        return "Raspberry Pi" in Path("/proc/device-tree/model").read_text(errors="replace")
    except OSError:
        return False


def needed():
    """True if the one-time optimisation (or an update of it) still has to run."""
    if os.geteuid() != 0 or not on_raspberry_pi():
        return False
    mark = _marker()
    return not (mark and mark.get("version") == VERSION)


def revert_needed():
    return os.geteuid() == 0 and on_raspberry_pi() and _marker() is not None


def wait_until_done(timeout=90):
    if RUNNING.is_set():
        end = time.monotonic() + timeout
        while RUNNING.is_set() and time.monotonic() < end:
            time.sleep(0.2)


def _set_unit(text):
    UNIT.write_text(text)
    _run("systemctl", "enable", "--no-reload", "zeroichi.service")
    _run("systemctl", "daemon-reload")


# ---------------------------------------------------------------- initramfs
def initramfs_check():
    """Is it safe to boot without the initramfs? Only if everything the kernel
    needs to reach the root filesystem on the SD card is built in."""
    import platform
    try:
        cmdline = CMDLINE_TXT.read_text()
        if any(x in cmdline for x in ("cryptdevice", "root=/dev/mapper", "root=/dev/sd",
                                      "root=/dev/nvme", "root=LABEL", "root=UUID")):
            return False, "unusual root device in cmdline.txt"
        root_fs = next((ln.split()[2] for ln in Path("/proc/mounts").read_text().splitlines()
                        if ln.split()[1] == "/"), "")
        if root_fs != "ext4":
            return False, f"root filesystem is {root_fs or 'unknown'}, not ext4"
        builtin = Path(f"/lib/modules/{platform.release()}/modules.builtin").read_text()
        for need in ("kernel/fs/ext4/ext4.ko", "kernel/drivers/mmc/core/mmc_block.ko"):
            if need not in builtin:
                return False, f"{need.split('/')[-1]} is not built into the kernel"
        if "kernel/drivers/mmc/host/" not in builtin:
            return False, "no SD card host driver built into the kernel"
        return True, "SD card and ext4 drivers are built into the kernel"
    except Exception as e:
        return False, f"check failed ({e})"


def _remove_fw_block(text):
    if FW_START not in text:
        return text
    i = text.index(FW_START)
    start = text.rfind("\n[all]\n", 0, i)
    start = start if start >= 0 and i - start < 12 else i
    end = text.index(FW_END, i) + len(FW_END) if FW_END in text[i:] else len(text)
    if end < len(text) and text[end] == "\n":
        end += 1
    return text[:start] + text[end:]


def _fw_block(no_initramfs):
    lines = ["", "[all]", FW_START, "disable_splash=1", "boot_delay=0", "initial_turbo=30"]
    if no_initramfs:
        lines.append("auto_initramfs=0")
    lines += list(FW_EXTRA) + [FW_END, ""]
    return "\n".join(lines)


# ---------------------------------------------------------------- main steps
def optimize():
    if not needed():
        return
    RUNNING.set()
    try:
        _optimize(_marker() or {})
    finally:
        RUNNING.clear()


def _optimize(prev):
    import json
    log.info("Boot optimizer v%d: starting (one time only)", VERSION)
    done = dict(prev)
    done["version"] = VERSION
    done.setdefault("disabled", [])

    if CLOUD_OFF.parent.is_dir() and not CLOUD_OFF.exists():
        CLOUD_OFF.touch()
        done["cloud_init"] = True
        log.info("Boot optimizer: cloud-init turned off")

    states = _unit_states()
    todo = [u for u in DISABLE if states.get(u) in ("enabled", "enabled-runtime")]
    if todo and _run("systemctl", "disable", "--no-reload", *todo):
        done["disabled"] += [u for u in todo if u not in done["disabled"]]
    log.info("Boot optimizer: turned off %s", ", ".join(todo) or "nothing new")

    if SWAP_DROPIN.parent.parent.is_dir():           # /etc/rpi exists: rpi-swap is used
        try:
            SWAP_DROPIN.parent.mkdir(parents=True, exist_ok=True)
            _write(SWAP_DROPIN, SWAP_TEXT)
            done["swap"] = True
            log.info("Boot optimizer: swap in RAM only (no more swap-file resizing at boot)")
        except OSError as e:
            log.warning("Boot optimizer: could not change swap: %s", e)

    try:
        DROPIN.parent.mkdir(parents=True, exist_ok=True)
        _write(DROPIN, DROPIN_TEXT)
        done["dropin"] = True
    except OSError as e:
        log.warning("Boot optimizer: could not limit shutdown time: %s", e)

    try:
        cams = []
        try:                                  # camera drivers this Pi actually uses
            cams = [ln.split()[0] for ln in Path("/proc/modules").read_text().splitlines()
                    if ln.strip() and CAMERA_MODULES.match(ln.split()[0])]
        except OSError:
            pass                              # still preload the screen driver
        _write(MODULES_CONF, MODULES_TEXT + "".join(m + "\n" for m in cams))
        done["modules"] = True
        log.info("Boot optimizer: screen%s drivers load at the start of boot%s",
                 " and camera" if cams else "", f" ({', '.join(cams)})" if cams else "")
    except OSError as e:
        log.warning("Boot optimizer: could not add early modules: %s", e)

    try:
        _write(SPIDEV_CONF, SPIDEV_TEXT)
        done["spidev"] = True
        log.info("Boot optimizer: SPI driver set to 64 KB writes")
    except OSError as e:
        log.warning("Boot optimizer: could not set the SPI buffer: %s", e)

    masked = [u for u in MASK if states.get(u) not in (None, "masked")]
    if masked and _run("systemctl", "mask", "--no-reload", *masked):
        done["masked"] = sorted(set(done.get("masked", [])) | set(masked))
        log.info("Boot optimizer: switched off %s", ", ".join(masked))

    try:
        import copysync
        copysync.sync(SD_DIR, COPY_DIR)
        done["copy"] = True
        log.info("Boot optimizer: Zero Ichi copy installed in %s", COPY_DIR)
    except Exception as e:
        log.warning("Boot optimizer: could not install the fast-start copy: %s", e)

    try:
        _set_unit(FAST_UNIT)
        log.info("Boot optimizer: Zero Ichi starts as soon as the main partition is ready")
    except OSError as e:
        log.warning("Boot optimizer: could not update service: %s", e)

    safe, why = initramfs_check()
    log.info("Boot optimizer: initramfs %s (%s)", "will be skipped" if safe else "kept", why)
    try:
        text = _remove_fw_block(CONFIG_TXT.read_text())
        _write(CONFIG_TXT, text.rstrip("\n") + "\n" + _fw_block(safe))
        done["firmware"] = True
        done["initramfs_off"] = safe
    except OSError as e:
        log.warning("Boot optimizer: could not update config.txt: %s", e)

    try:
        raw = CMDLINE_TXT.read_text()
        tokens = raw.split()
        added = [t for t in CMDLINE_ADD
                 if t not in tokens and not (t.startswith("loglevel=")
                                             and any(x.startswith("loglevel=") for x in tokens))]
        if added and tokens:
            _write(CMDLINE_TXT, " ".join(tokens + added) + "\n")
            log.info("Boot optimizer: kernel messages quietened (%s)", " ".join(added))
        done["cmdline_added"] = sorted(set(done.get("cmdline_added", [])) | set(added))
    except OSError as e:
        log.warning("Boot optimizer: could not update cmdline.txt: %s", e)

    MARK.parent.mkdir(parents=True, exist_ok=True)
    MARK.write_text(json.dumps(done))
    os.sync()
    log.info("Boot optimizer: done - takes effect from the next boot")


# ---------------------------------------------------------------- fast start
def pid1():
    try:
        return Path("/proc/1/comm").read_text().strip()
    except OSError:
        return ""


def fast_start_running():
    """True when this start is Zero Ichi's own fast start (not systemd)."""
    return os.environ.get("ZEROICHI_FASTINIT") == "1"       # (set by fast-start.sh)


def record_modules():
    """Remember the drivers this Pi uses - read during a normal start-up, when
    the system has loaded everything. The fast start loads exactly these."""
    if pid1() != "systemd":
        return False
    try:
        names = [ln.split()[0] for ln in PROC_MODULES.read_text().splitlines() if ln.strip()]
    except OSError:
        return False
    names = sorted(n for n in names if not SKIP_MODULES.match(n))
    if not any("unicam" in n for n in names):    # camera drivers not there (yet): keep the old
        return MODULES_LIST.exists()
    text = "\n".join(names) + "\n"
    try:
        if not MODULES_LIST.exists() or MODULES_LIST.read_text() != text:
            MODULES_LIST.parent.mkdir(parents=True, exist_ok=True)
            _write(MODULES_LIST, text)
            log.info("Fast start: driver list updated (%d drivers)", len(names))
        return True
    except OSError as e:
        log.warning("Fast start: could not save the driver list: %s", e)
        return False


# (v50) The start-up that is running (the Pi's first process) keeps reading
# its script until the Pi is switched off. Replaced by a new one, the old file
# would be "deleted but still in use" - and then the system partition can't
# be made read-only (see fast-start.sh, system_ro) for the rest of that run,
# and the next start has to tidy it up first. So the running one keeps a
# second name until a later update finds nothing runs it any more.
INIT_OLD = INIT_DST.with_name(".zeroichi-init-old")


def _pid1_files():
    """(device, inode) of every file the first process has open."""
    found = set()
    try:
        for fd in os.listdir("/proc/1/fd"):
            try:
                st = os.stat(f"/proc/1/fd/{fd}")
                found.add((st.st_dev, st.st_ino))
            except OSError:
                pass
    except OSError:
        pass
    return found


def _in_use(path, files):
    try:
        st = path.stat()
    except OSError:
        return False
    return (st.st_dev, st.st_ino) in files


def _drop_old_init():
    """The kept old script, once nothing runs it."""
    if INIT_OLD.exists() and not _in_use(INIT_OLD, _pid1_files()):
        try:
            INIT_OLD.unlink()
        except OSError:
            pass


def _keep_running_init():
    """Before the script is replaced: if it is the one running, it keeps a name."""
    files = _pid1_files()
    if not _in_use(INIT_DST, files) or _in_use(INIT_OLD, files):
        return
    try:
        INIT_OLD.unlink(missing_ok=True)
        os.link(INIT_DST, INIT_OLD)
    except OSError as e:
        log.info("Fast start: the running start-up script could not keep a name (%s)", e)


def install_init(app_dir):
    """Put fast-start.sh in place as /usr/local/sbin/zeroichi-init (when it is new
    or changed) - only after checking it is complete and has no syntax errors:
    a broken start-up script would stop the Pi from starting."""
    src = next((d / INIT_SRC for d in (Path(app_dir), SD_DIR) if (d / INIT_SRC).is_file()), None)
    if src is None:
        return INIT_DST.exists()
    try:
        text = src.read_text()
    except OSError:
        return INIT_DST.exists()
    if not text.startswith("#!/bin/sh") or INIT_MARKER not in text \
            or not text.rstrip().endswith("normal_start"):
        log.warning("Fast start: %s looks incomplete - not used", src)
        return INIT_DST.exists()
    try:
        _drop_old_init()
        if INIT_DST.exists() and INIT_DST.read_text() == text:
            return True
        import subprocess
        if subprocess.run(["sh", "-n", str(src)], capture_output=True).returncode != 0:
            log.warning("Fast start: %s has an error - not used", src)
            return INIT_DST.exists()
        INIT_DST.parent.mkdir(parents=True, exist_ok=True)
        tmp = INIT_DST.with_name(INIT_DST.name + ".zeroichi-tmp")
        with open(tmp, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o755)
        _keep_running_init()
        os.replace(tmp, INIT_DST)
        os.sync()
        log.info("Fast start: start-up script installed (%s)", INIT_DST)
        return True
    except OSError as e:
        log.warning("Fast start: could not install the start-up script: %s", e)
        return False


def install_lcd_helper(app_dir):
    """Put zeroichi-lcd (the tiny program that shows the start-up picture at once)
    in place, when it is new or changed. Only a program for this Pi's processor
    (32-bit ARM) is accepted."""
    src = next((d / LCD_SRC for d in (Path(app_dir), SD_DIR) if (d / LCD_SRC).is_file()), None)
    if src is None:
        return
    try:
        data = src.read_bytes()
        if data[:4] != b"\x7fELF" or data[4] != 1 or int.from_bytes(data[18:20], "little") != 40:
            log.warning("Fast start: %s is not a Pi program - not used", src)
            return
        if LCD_DST.exists() and LCD_DST.read_bytes() == data:
            return
        LCD_DST.parent.mkdir(parents=True, exist_ok=True)
        tmp = LCD_DST.with_name(LCD_DST.name + ".zeroichi-tmp")
        with open(tmp, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o755)
        os.replace(tmp, LCD_DST)
        log.info("Fast start: start-up picture program installed (%s)", LCD_DST)
    except OSError as e:
        log.warning("Fast start: could not install %s: %s", LCD_SRC, e)


# For now (v43): the kernel notes how long each of its start-up steps takes
# (boot-time.txt lists the slowest) - to find what else can be cut. Costs a
# few hundredths of a second; off again once that's known.
KERNEL_TIMING = ("initcall_debug", "log_buf_len=1M")
KERNEL_TIMING_ON = True


def _cmdline_timing(on):
    try:
        tokens = CMDLINE_TXT.read_text().split()
    except OSError:
        return
    if not tokens:
        return
    have = [t for t in KERNEL_TIMING if t in tokens]
    if (on and len(have) == len(KERNEL_TIMING)) or (not on and not have):
        return
    new = [t for t in tokens if t not in KERNEL_TIMING] + (list(KERNEL_TIMING) if on else [])
    try:
        _write(CMDLINE_TXT, " ".join(new) + "\n")
        os.sync()
        log.info("Kernel start-up timing %s from the next start", "on" if on else "off")
    except OSError as e:
        log.warning("Could not update cmdline.txt: %s", e)


# v45: two kernel start-up steps that boot-time.txt showed cost a Pi Zero about
# 2.6 s, and that Zero Ichi doesn't need:
#  - trace_eval_init (about 1.7 s): it tidies up the kernel's tracing tables -
#    only of use to kernel tracing tools - so it is skipped;
#  - the USB driver dwc_otg starting up (about 0.9 s, even with nothing plugged
#    in): the newer dwc2 driver is used instead (dtoverlay=dwc2) - it loads when
#    needed (the fast start: after the camera), USB works as before.
KERNEL_SKIP = ("initcall_blacklist=trace_eval_init",)
FW_EXTRA = ("dtoverlay=dwc2",)


def _cmdline_skip(on):
    try:
        tokens = CMDLINE_TXT.read_text().split()
    except OSError:
        return
    if not tokens:
        return
    have = [t for t in KERNEL_SKIP if t in tokens]
    if (on and len(have) == len(KERNEL_SKIP)) or (not on and not have):
        return
    new = [t for t in tokens if t not in KERNEL_SKIP] + (list(KERNEL_SKIP) if on else [])
    try:
        _write(CMDLINE_TXT, " ".join(new) + "\n")
        os.sync()
        log.info("Kernel start-up: tracing tables %s from the next start", "skipped" if on else "back")
    except OSError as e:
        log.warning("Could not update cmdline.txt: %s", e)


def _fw_extra(on):
    """FW_EXTRA in (or out of) Zero Ichi's own part of config.txt (the fast boot
    block - made by the boot optimizer); not added if config.txt has it already."""
    try:
        text = CONFIG_TXT.read_text()
    except OSError:
        return
    if FW_START not in text or FW_END not in text:
        return
    i = text.index(FW_START)
    j = text.index(FW_END, i)
    block = text[i:j].split("\n")
    others = (text[:i] + text[j:]).split("\n")
    want = [x for x in FW_EXTRA if on and x not in [o.strip() for o in others]]
    body = [ln for ln in block if ln.strip() not in FW_EXTRA]
    while body and not body[-1].strip():
        body.pop()
    body += want
    new = text[:i] + "\n".join(body) + "\n" + text[j:]
    if new == text:
        return
    try:
        _write(CONFIG_TXT, new)
        os.sync()
        log.info("Start-up: USB driver %s from the next start", "dwc2" if want else "back to dwc_otg")
    except OSError as e:
        log.warning("Could not update config.txt: %s", e)


# (0.1.0) Zero Ichi never uses Wi-Fi or Bluetooth - by design, an offline
# camera. On a Pi with radios (Zero W, Zero 2 W) the Pi's firmware switches
# them off before Linux even starts: they don't exist for the system at all.
# The Pi Zero has none ([pi0w] / [pi02]: only those boards read these lines).
RADIO_START, RADIO_END = "# Zero Ichi: no Wi-Fi, no Bluetooth", "# Zero Ichi: no Wi-Fi, no Bluetooth end"
RADIO_BLOCK = "\n".join(["", RADIO_START, "[pi0w]", "dtoverlay=disable-wifi", "dtoverlay=disable-bt",
                         "[pi02]", "dtoverlay=disable-wifi", "dtoverlay=disable-bt", "[all]", RADIO_END, ""])


def radios_off():
    """Makes sure config.txt has RADIO_BLOCK (at every start: an update, a
    card image, config.txt edited...). Takes effect from the next start."""
    if os.geteuid() != 0 or not on_raspberry_pi():
        return
    try:
        text = CONFIG_TXT.read_text()
    except OSError:
        return
    if RADIO_START in text and RADIO_END in text:
        return
    try:
        _write(CONFIG_TXT, text.rstrip("\n") + "\n" + RADIO_BLOCK)
        os.sync()
        log.info("Wi-Fi and Bluetooth: switched off for good (from the next start)")
    except OSError as e:
        log.warning("Could not switch off Wi-Fi and Bluetooth in config.txt: %s", e)


def _cmdline_init(on):
    """Add or remove "init=/usr/local/sbin/zeroichi-init" in cmdline.txt."""
    try:
        tokens = CMDLINE_TXT.read_text().split()
    except OSError as e:
        log.warning("Fast start: could not read cmdline.txt: %s", e)
        return
    if not tokens:
        return
    has = INIT_TOKEN in tokens
    other = [t for t in tokens if t.startswith("init=") and t != INIT_TOKEN]
    if on and not has:
        if other:
            log.info("Fast start: not switched on (cmdline.txt already has %s)", other[0])
            return
        tokens.append(INIT_TOKEN)
    elif not on and has:
        tokens = [t for t in tokens if t != INIT_TOKEN]
    else:
        return
    try:
        _write(CMDLINE_TXT, " ".join(tokens) + "\n")
        os.sync()
        log.info("Fast start: %s from the next start", "on" if on else "off")
    except OSError as e:
        log.warning("Fast start: could not update cmdline.txt: %s", e)


# ------------------------------------------------ v53: the new names (once)
# Until v52 Zero Ichi's own files still had its first name, ToyCam: the card's
# "toycam" folder, /var/lib/toycam, the start-up script toycam-init... (the
# Pi looks for some of them at the very start of switching on, before Zero
# Ichi runs, so they couldn't simply be renamed in an update). The update to
# v53 is copied into that "toycam" folder as usual; v52's own update step
# (toycam.py --maintenance) ends by calling sync_fast_start below - this, the
# new version's - and migrate_names() moves everything to the new names, each
# step checked before the next one:
#   1. what the new start-up needs: the Pi's own Zero Ichi settings in
#      /var/lib/zeroichi (screen, drivers...), the copy of Zero Ichi in
#      /var/lib/zeroichi/app, the start-up script zeroichi-init, zeroichi-lcd;
#   2. cmdline.txt: the Pi starts with zeroichi-init (read back to check);
#   3. Zero Ichi's other system files (its service, small settings files,
#      config.txt's fast boot lines, the Pi's name);
#   4. the card's folder renamed "zeroichi" (and the drive TOYCAM - if it is
#      called that - is renamed ZEROICHI at the next start, just before it is
#      opened: fast-start.sh);
#   5. the old start-up restarts the Pi once, into the new one: the program it
#      runs next is replaced by one that only says "restart" (exit code 66).
# If a step isn't possible, nothing is switched: v52 carries on as before (its
# program still works with this camera server - camd.py) and the log says
# why. The new start-up removes the old name's files at its first start.
OLD_SD = Path("/boot/firmware/toycam")
OLD_STATE = Path("/var/lib/toycam")
OLD_INIT_TOKEN = "init=/usr/local/sbin/toycam-init"
OLD_UNIT = Path("/etc/systemd/system/toycam.service")
WANTS_DIR = Path("/etc/systemd/system/multi-user.target.wants")
OLD_CONFS = ((Path("/etc/systemd/system.conf.d/toycam-fast-shutdown.conf"), DROPIN),
             (Path("/etc/rpi/swap.conf.d/80-toycam-zram-only.conf"), SWAP_DROPIN),
             (Path("/etc/modules-load.d/toycam.conf"), MODULES_CONF),
             (Path("/etc/modprobe.d/toycam-spidev.conf"), SPIDEV_CONF))
OLD_FW = ("# ToyCam fast boot", "# ToyCam fast boot end")
OLD_PROGRAMS = ("toycam.py", "toycam-c", "toycam-lcd", "src/toycam-c", "src/toycam-lcd.c")
STATE_DIR = MARK.parent
CACHE_DIR = Path("/var/cache/zeroichi/lib")
CACHE_MODULES = ("config", "copysync", "bootopt", "camd")     # (what the C program's start uses)
RELABEL_FLAG = STATE_DIR / "fat-relabel"
HOSTNAME = Path("/etc/hostname")
HOSTS = Path("/etc/hosts")
SERIAL = Path("/proc/device-tree/serial-number")
RUN_OLD = Path("/run/toycam")


def _old_system():
    """Does this Pi still start Zero Ichi under the old name?"""
    try:
        tokens = CMDLINE_TXT.read_text().split()
    except OSError:
        tokens = []
    return OLD_INIT_TOKEN in tokens or (OLD_UNIT.exists() and not UNIT.exists())


def _kit_problem(d):
    """"" when folder d has the new version complete, else what is missing."""
    try:
        head = (d / "zeroichi-c").read_bytes()[:20]
        text = (d / INIT_SRC).read_text()
    except OSError:
        return "zeroichi-c or fast-start.sh is missing"
    if len(head) < 20 or head[:4] != b"\x7fELF" or head[4] != 1 \
            or int.from_bytes(head[18:20], "little") != 40:
        return "zeroichi-c is not a program for this Pi"
    if not text.startswith("#!/bin/sh") or INIT_MARKER not in text \
            or not text.rstrip().endswith("normal_start"):
        return "fast-start.sh is incomplete"
    for n in ("zeroichi.py", "camd.py", "camd_start.py", "config.py", "copysync.py", LCD_SRC):
        if not (d / n).is_file():
            return f"{n} is missing"
    return ""


def _copy_state():
    """The Pi's own settings (screen, driver list, boot changes...) - not the
    old copy of Zero Ichi (made again below), nor a "normal start" request.
    (The old ones are the ones in use until the switch: they win.)"""
    import shutil
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    for p in sorted(OLD_STATE.iterdir()) if OLD_STATE.is_dir() else ():
        d = STATE_DIR / p.name
        if p.name in ("app", "normal-start") or p.name.endswith(".tmp") or p.is_symlink():
            continue
        if p.is_dir():
            shutil.copytree(p, d, symlinks=True, dirs_exist_ok=True)
        elif p.is_file():
            shutil.copy2(p, d)


def _build_copy(src):
    """The copy of Zero Ichi the fast start runs (the same files copysync keeps
    in step with the card - so the first start finds nothing to update)"""
    import shutil
    COPY_DIR.mkdir(parents=True, exist_ok=True)
    for p in sorted(src.iterdir()):
        n = p.name
        if n.startswith((".", "toycam")) or not p.is_file():
            continue
        if not (p.suffix in (".py", ".rgb565") or n in ("settings.txt", "zeroichi-c")):
            continue
        tmp = COPY_DIR / (n + ".tmp")
        shutil.copy2(p, tmp)
        if n == "zeroichi-c":
            os.chmod(tmp, 0o755)
        os.replace(tmp, COPY_DIR / n)
    os.sync()


def _compile_cache():
    """The camera server ready-compiled (the start waits for it - camd_start.py)"""
    import py_compile
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    for name in CACHE_MODULES:
        s, c = COPY_DIR / f"{name}.py", CACHE_DIR / f"{name}.pyc"
        try:
            py_compile.compile(str(s), cfile=str(c), dfile=str(s), doraise=True)
        except Exception:
            c.unlink(missing_ok=True)            # (then from the source - slower, works)


def _rename_system():
    """Zero Ichi's service and small system files under the new names. Each one
    on its own: a failure is only noted (none of them is needed to start)."""
    if OLD_UNIT.exists():
        try:
            fast = "DefaultDependencies=no" in OLD_UNIT.read_text()
            if not UNIT.exists():
                _write(UNIT, FAST_UNIT if fast else NORMAL_UNIT)
            old_link = WANTS_DIR / OLD_UNIT.name
            if old_link.is_symlink() and not (WANTS_DIR / UNIT.name).is_symlink():
                (WANTS_DIR / UNIT.name).symlink_to(UNIT)       # (= systemctl enable)
            old_link.unlink(missing_ok=True)
            OLD_UNIT.unlink()
        except OSError as e:
            log.warning("New names: service: %s", e)
    for old, new in OLD_CONFS:
        try:
            if old.exists():
                if not new.exists():
                    new.parent.mkdir(parents=True, exist_ok=True)
                    _write(new, old.read_text().replace("ToyCam", "Zero Ichi"))
                old.unlink()
        except OSError as e:
            log.warning("New names: %s: %s", old, e)
    try:                                         # config.txt: "# ToyCam fast boot" (until v50)
        text = CONFIG_TXT.read_text()
        lines = text.split("\n")
        marks = [ln.strip() for ln in lines]
        if OLD_FW[0] in marks:
            if FW_START in marks:                # (both: the old block is a leftover)
                i = marks.index(OLD_FW[0])
                j = marks.index(OLD_FW[1], i) if OLD_FW[1] in marks[i:] else i
                lines = lines[:i] + lines[j + 1:]
            else:
                lines = [FW_START if m == OLD_FW[0] else FW_END if m == OLD_FW[1] else ln
                         for ln, m in zip(lines, marks)]
            _write(CONFIG_TXT, "\n".join(lines))
    except (OSError, ValueError) as e:
        log.warning("New names: config.txt: %s", e)
    try:
        if HOSTNAME.read_text().strip() == "toycam":
            _write(HOSTNAME, "zeroichi\n")
            hosts = HOSTS.read_text()
            new = re.sub(r"(?m)(?<=\s)toycam(?=\s|$)", "zeroichi", hosts)
            if new != hosts:
                _write(HOSTS, new)
    except OSError as e:
        log.warning("New names: the Pi's name: %s", e)
    os.sync()


def _switch_cmdline():
    """cmdline.txt: start with zeroichi-init instead of toycam-init (read back
    to check; the old line again if it didn't come out right)."""
    try:
        raw = CMDLINE_TXT.read_text()
    except OSError as e:
        log.warning("New names: not now - cmdline.txt can't be read: %s", e)
        return False
    tokens = raw.split()
    if OLD_INIT_TOKEN not in tokens:
        return True                              # (no fast start: nothing to switch)
    new = [INIT_TOKEN if t == OLD_INIT_TOKEN else t for t in tokens if t != INIT_TOKEN]
    try:
        _write(CMDLINE_TXT, " ".join(new) + "\n")
        os.sync()
        if CMDLINE_TXT.read_text().split() == new:
            return True
    except OSError as e:
        log.warning("New names: cmdline.txt: %s", e)
    try:
        _write(CMDLINE_TXT, raw)
        os.sync()
    except OSError:
        pass
    log.warning("New names: not now - cmdline.txt could not be changed")
    return False


def _rename_folder(src):
    """The card's folder: toycam -> zeroichi (and the old programs in it out)"""
    import shutil
    if src == OLD_SD:
        if SD_DIR.exists():                      # (a zeroichi folder already? kept, renamed)
            spare = next(SD_DIR.with_name(f"zeroichi-old{i or ''}") for i in range(1000)
                         if not SD_DIR.with_name(f"zeroichi-old{i or ''}").exists())
            SD_DIR.rename(spare)
            log.info("New names: the card had a zeroichi folder already - it is now called %s",
                     spare.name)
        OLD_SD.rename(SD_DIR)
    for n in OLD_PROGRAMS:
        p = SD_DIR / n
        if p.is_dir():
            shutil.rmtree(p, ignore_errors=True)
        else:
            p.unlink(missing_ok=True)
    os.sync()


def _restart_picture():
    """While the old start-up restarts the Pi it shows /run/toycam/message.rgb565:
    the new start-up picture (the one that fits this Pi's screen)"""
    import shutil
    conf = STATE_DIR / "lcd.conf"
    try:
        serial = re.sub(r"[^0-9a-fA-F]", "", SERIAL.read_text(errors="replace"))
        if serial and (STATE_DIR / f"lcd-{serial}.conf").is_file():
            conf = STATE_DIR / f"lcd-{serial}.conf"
    except OSError:
        pass
    try:
        size = {"width": 240, "height": 240}
        for ln in conf.read_text().splitlines():
            k = ln.split()
            if len(k) >= 2 and k[0] in size:
                size[k[0]] = int(k[1], 0)
        want = size["width"] * size["height"] * 2
        for p in sorted(COPY_DIR.glob("splash*.rgb565")):
            if p.stat().st_size == want and RUN_OLD.is_dir():
                shutil.copyfile(p, RUN_OLD / "message.rgb565")
                return
    except (OSError, ValueError):
        pass


def _restart_once():
    """v52's start-up runs its program again next: a 3-line one that asks it
    to restart the Pi (exit code 66 = "restart")."""
    stub = OLD_STATE / "app" / "toycam-c"
    if os.environ.get("TOYCAM_FASTINIT") != "1" or not stub.parent.is_dir():
        log.info("New names: done - in use from the next start")
        return
    _restart_picture()
    tmp = stub.with_name("toycam-c.tmp")
    tmp.write_text("#!/bin/sh\n# (v53) Zero Ichi has its new names now: this asks the old start-up\n"
                   "# to restart the Pi once - into the new one, zeroichi-init.\nexit 66\n")
    os.chmod(tmp, 0o755)
    os.replace(tmp, stub)
    os.sync()
    log.info("New names: done - the Pi restarts once, into the new start-up")


def migrate_names():
    """None: nothing to do (the usual); True: Zero Ichi has its new names now;
    False: not possible this time (everything stays as it was)."""
    if not (OLD_SD.is_dir() or OLD_STATE.is_dir() or OLD_UNIT.exists()) or not _old_system():
        return None
    src = OLD_SD if (OLD_SD / "zeroichi-c").exists() else SD_DIR
    why = _kit_problem(src)
    if why:
        log.warning("New names: not now - %s (in %s)", why, src)
        return False
    log.info("New names: moving Zero Ichi from its old name, ToyCam (once)")
    try:
        _copy_state()
        _build_copy(src)
        _compile_cache()
        RELABEL_FLAG.write_text("TOYCAM -> ZEROICHI\n")
    except OSError as e:
        log.warning("New names: not now - the copy of Zero Ichi could not be made: %s", e)
        return False
    install_lcd_helper(src)
    if not install_init(src) or not os.access(INIT_DST, os.X_OK):
        log.warning("New names: not now - the new start-up script could not be installed")
        return False
    if not _switch_cmdline():
        return False
    _rename_system()
    try:
        _rename_folder(src)
        log.info("New names: the card's folder is called zeroichi now")
    except OSError as e:                         # (fast-start.sh renames it then)
        log.warning("New names: the card's folder: %s - renamed at the next start", e)
    try:
        _restart_once()
    except OSError as e:
        log.warning("New names: could not restart by itself (%s) - switch off and on", e)
    return True


def sync_fast_start(enabled, app_dir):
    """At every start: switch the fast start on or off to match settings.txt
    (takes effect from the next start), keep its script and driver list fresh."""
    if os.geteuid() != 0 or not on_raspberry_pi():
        return
    moved = migrate_names()                      # (v53: once)
    if moved is False:
        return                                   # (everything as it was - v52 carries on)
    if moved:
        app_dir = COPY_DIR
    if pid1() == "systemd" and NORMAL_FLAG.exists():
        NORMAL_FLAG.unlink(missing_ok=True)          # (the one normal start has happened)
        log.info("Fast start: normal start-up done - the next start is fast again")
    have_list = record_modules() or MODULES_LIST.exists()
    install_lcd_helper(app_dir)
    on = bool(enabled) and have_list and install_init(app_dir)
    _cmdline_init(on)
    _cmdline_timing(on and KERNEL_TIMING_ON)
    _cmdline_skip(on)
    _fw_extra(on)


def revert():
    mark = _marker()
    if os.geteuid() != 0 or not on_raspberry_pi() or not mark:
        return
    log.info("Boot optimizer: undoing changes (optimize_boot = no)")
    if mark.get("cloud_init"):
        CLOUD_OFF.unlink(missing_ok=True)
    for f, key in ((DROPIN, "dropin"), (SWAP_DROPIN, "swap"), (MODULES_CONF, "modules"),
                   (SPIDEV_CONF, "spidev")):
        if mark.get(key):
            f.unlink(missing_ok=True)
    if mark.get("disabled"):
        _run("systemctl", "enable", "--no-reload", *mark["disabled"])
    if mark.get("masked"):
        _run("systemctl", "unmask", "--no-reload", *mark["masked"])
    if mark.get("copy"):
        import shutil
        shutil.rmtree(COPY_DIR, ignore_errors=True)
    try:
        _set_unit(NORMAL_UNIT)
    except OSError as e:
        log.warning("Boot optimizer: could not restore service: %s", e)
    try:
        text = CONFIG_TXT.read_text()
        if FW_START in text:
            _write(CONFIG_TXT, _remove_fw_block(text))
    except (OSError, ValueError) as e:
        log.warning("Boot optimizer: could not restore config.txt: %s", e)
    try:
        added = mark.get("cmdline_added") or []
        if added:
            tokens = [t for t in CMDLINE_TXT.read_text().split() if t not in added]
            _write(CMDLINE_TXT, " ".join(tokens) + "\n")
    except OSError as e:
        log.warning("Boot optimizer: could not restore cmdline.txt: %s", e)
    _cmdline_init(False)
    _cmdline_timing(False)
    _cmdline_skip(False)
    INIT_DST.unlink(missing_ok=True)
    _drop_old_init()
    LCD_DST.unlink(missing_ok=True)
    MARK.unlink(missing_ok=True)
    os.sync()
    log.info("Boot optimizer: undone - back to normal from the next boot")
