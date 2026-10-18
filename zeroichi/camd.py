"""Zero Ichi camera server: runs the camera for Zero Ichi's C program.

The camera library (libcamera) is C++, so Zero Ichi's C program uses it through
this small helper, which only loads the camera library itself (none of numpy,
Pillow or picamera2 - that's what took most of the old start-up time).

It talks to Zero Ichi over the socket it was started with (file descriptor 3),
one short text message per packet; frame buffers are passed once as file
descriptors and Zero Ichi reads the pictures straight from them.

"camd.py --listen": started by the fast start before Zero Ichi itself, so the
camera library loads (the slowest part of the start) while the drivers load
and the screen starts. It finds the camera, then waits for Zero Ichi to connect
(socket "@zeroichi-camd"); messages meanwhile are kept until then.

  Zero Ichi -> here:  CONF mw mh lw lh hflip vflip buffers fps
                   DONE cookie | CTRL name value... | STOP | START | QUIT
  here -> Zero Ichi:  READY seconds... | CONFIGURED ... | REQ ... (+ fds)
                   FRAME cookie seq timestamp_us exposure_us gain lux colour_temp
                   LOG text | ERROR text
"""
import os
import sys

# Started with "python3 -S" (skips scanning for add-on packages - slow on a Pi
# Zero): add the system package folders, where the camera library is.
if sys.flags.no_site:
    _v = f"python{sys.version_info.major}.{sys.version_info.minor}"
    for _p in (f"/usr/local/lib/{_v}/dist-packages", "/usr/lib/python3/dist-packages",
               f"/usr/lib/{_v}/dist-packages"):
        if os.path.isdir(_p) and _p not in sys.path:
            sys.path.append(_p)

import _socket  # noqa: E402  (not "socket": it loads a dozen other modules - slow on a Pi Zero)
import select  # noqa: E402
import time  # noqa: E402

T0 = time.monotonic()
LISTEN = "--listen" in sys.argv
PENDING = []                                      # (listening: sent once Zero Ichi is here)
# (v53) Started by v52's start-up - before its one-time move to Zero Ichi's new
# names (bootopt.migrate_names; only if that move wasn't possible): the old
# names, so v52's program still finds its camera server.
OLD_NAMES = os.environ.get("TOYCAM_FASTINIT") == "1" and os.environ.get("ZEROICHI_FASTINIT") != "1"
RUN = "/run/toycam" if OLD_NAMES else "/run/zeroichi"
FASTINIT = OLD_NAMES or os.environ.get("ZEROICHI_FASTINIT") == "1"
BOOT_LOG = RUN + "/fast-start.txt"


def bootmark(text):
    """Fast start: note a step in boot-time.txt's list (as the fast start's own
    "mark": seconds since the kernel started, the step, the idle time so far)."""
    if not LISTEN or not os.path.exists(BOOT_LOG):
        return
    try:
        with open("/proc/uptime") as f:
            up, idle = f.read().split()[:2]
        with open(BOOT_LOG, "a") as f:
            f.write(f"{up} camera server: {text} @{idle}\n")
    except (OSError, ValueError):
        pass


bootmark("Python ready")
if LISTEN:
    SOCK = None
    _LS = _socket.socket(_socket.AF_UNIX, _socket.SOCK_SEQPACKET)
    _LS.bind("\0toycam-camd" if OLD_NAMES else "\0zeroichi-camd")
    _LS.listen(1)
else:
    SOCK = _socket.socket(fileno=3)


def await_client(timeout=60):
    """Listening: wait for Zero Ichi (nobody within `timeout` s: end - the
    camera is let go)."""
    global SOCK
    if SOCK is not None:
        return
    _LS.settimeout(timeout)
    try:
        fd, _ = _LS._accept()
    except OSError:
        os._exit(0)
    _LS.close()
    conn = _socket.socket(fileno=fd)
    conn.settimeout(None)
    SOCK = conn
    for text, fds in PENDING:
        send(text, fds)
    PENDING.clear()


def send(text, fds=()):
    if SOCK is None:
        PENDING.append((text, list(fds)))
        return
    data = text.encode()
    try:
        if fds:
            import array
            SOCK.sendmsg([data], [(_socket.SOL_SOCKET, _socket.SCM_RIGHTS, array.array("i", list(fds)))])
        else:
            SOCK.send(data)
    except OSError:
        os._exit(0)                              # Zero Ichi has gone


def log(text):
    send("LOG " + text)


def _ls(d, prefix=""):
    """The paths in folder d (whose names start with prefix) - like glob, which
    loads the slow "re" module."""
    try:
        return [f"{d}/{n}" for n in os.listdir(d) if n.startswith(prefix)]
    except OSError:
        return []


def _device_name(node):
    base = os.path.basename(node)
    for p in (f"/sys/class/video4linux/{base}/name", f"/sys/bus/media/devices/{base}/model"):
        try:
            with open(p) as f:
                return f.read().strip()
        except OSError:
            continue
    return ""


def register_devices():
    """Fast start: the device manager (udev) doesn't run, so register the video
    devices the way it would (an entry in its database) - the camera library
    only uses registered devices. Done by the fast start as well; this covers
    devices that appear later."""
    if (not FASTINIT or os.path.exists("/run/udev/control")
            or os.geteuid() != 0):
        return
    for d in _ls("/sys/class/video4linux") + _ls("/sys/bus/media/devices"):
        try:
            with open(d + "/dev") as f:
                mm = f.read().strip()
            p = f"/run/udev/data/c{mm}"
            if not os.path.exists(p):
                os.makedirs("/run/udev/data", exist_ok=True)
                with open(p, "w") as f:
                    f.write(f"I:{int(time.monotonic() * 1e6)}\n")
        except OSError:
            continue


def wait_for_devices(timeout=8.0):
    """The camera library only uses devices the device manager (udev) has
    finished with: wait for the camera's own ones (not the video codec).
    (Once the fast start says its drivers are all loaded, the first complete
    look is enough.)"""
    end = time.monotonic() + timeout
    stable, last = 0, None
    while time.monotonic() < end:
        register_devices()
        nodes = sorted(n for n in _ls("/dev", "media") + _ls("/dev", "video") + _ls("/dev", "v4l-subdev")
                       if "codec" not in _device_name(n))
        missing = 0
        for n in nodes:
            try:
                rdev = os.stat(n).st_rdev
            except OSError:
                missing += 1
                continue
            if not os.path.exists(f"/run/udev/data/c{os.major(rdev)}:{os.minor(rdev)}"):
                missing += 1
        names = [_device_name(n) for n in nodes]
        ok = (bool(nodes) and not missing and any("unicam" in n for n in names)
              and any(n.startswith("bcm2835-isp") for n in names)
              and any(n and not n.startswith(("unicam", "bcm2835")) for n in names))
        stable = stable + 1 if (ok and nodes == last) else 0
        last = nodes
        if stable >= 2 or (ok and os.path.exists(RUN + "/devices-ready")):
            return True
        time.sleep(0.1)
    return False


class Server:
    def __init__(self, lc):
        self.lc = lc
        self.cm = None
        self.cam = None
        self.cfg = None
        self.alloc = None
        self.reqs = []
        self.where = []                  # per request: "idle" | "queued" | "client"
        self.running = False
        self.controls = {}               # set on the next requests (and at start)
        self.sticky = {}                 # (v53) every control asked for (not the frame rate):
                                         #  set again at each start - e.g. after a new set-up
        self.fps = 15
        self.fdl = None                  # (v53) frame time limits (min, max) in us - None: 1/fps
        self.frames = 0
        self.meta = {}

    # -- finding the camera ------------------------------------------------
    def open(self):
        lc = self.lc
        t = time.monotonic()
        waited = wait_for_devices() if not os.environ.get("ZEROICHI_SIM") else True
        t_dev = time.monotonic() - t
        bootmark("camera's devices ready" if waited else "camera's devices not ready - trying anyway")
        deadline = time.monotonic() + 30
        tries = 0
        while True:
            t = time.monotonic()
            cm = lc.CameraManager.singleton()
            t_cm = time.monotonic() - t
            cams = cm.cameras
            if cams:
                break
            tries += 1
            del cams, cm                         # (a fresh look next time)
            import gc
            gc.collect()
            if time.monotonic() > deadline:
                send("ERROR no camera found")
                return False
            if tries in (1, 5) or tries % 10 == 0:
                log(f"Camera not found yet - retrying ({'devices ready' if waited else 'devices not ready'})")
            time.sleep(0.5)
        self.cm = cm
        self.cam = cams[0]
        bootmark("camera found")
        for i in range(30):                      # (busy: an earlier camera user is just ending)
            try:
                self.cam.acquire()
                break
            except (OSError, RuntimeError) as e:     # (the library reports "busy" as RuntimeError)
                if i == 29:
                    raise
                if i == 0:
                    log(f"camera busy ({e}) - waiting")
                time.sleep(0.2)
        send(f"READY {time.monotonic() - T0:.2f} devices {t_dev:.2f} manager {t_cm:.2f} tries {tries}"
             f" id {self.cam.id.replace(' ', '_')}")
        return True

    # -- set-up ------------------------------------------------------------
    def stop(self):
        if self.running:
            self.cam.stop()
            self.running = False
            self.drain()                         # (cancelled requests come back)
        for i, w in enumerate(self.where):
            if w == "queued":
                self.where[i] = "idle"

    def configure(self, mw, mh, lw, lh, hflip, vflip, nbuf, fps):
        lc = self.lc
        t0 = time.monotonic()
        self.stop()
        self.reqs, self.where, self.alloc, self.cfg = [], [], None, None
        import gc
        gc.collect()                             # (old buffers freed before configuring)
        self.fps = fps
        self.fdl = None
        roles = [lc.StreamRole.Viewfinder] + ([lc.StreamRole.Viewfinder] if lw else [])
        cfg = self.cam.generate_configuration(roles)
        O = lc.Orientation
        cfg.orientation = {(0, 0): O.Rotate0, (1, 0): O.Rotate0Mirror, (0, 1): O.Rotate180Mirror,
                           (1, 1): O.Rotate180}[(int(hflip), int(vflip))]
        for i, (w, h) in enumerate([(mw, mh)] + ([(lw, lh)] if lw else [])):
            s = cfg.at(i)
            s.size = lc.Size(w, h)
            s.pixel_format = lc.PixelFormat("YUV420")
            s.buffer_count = nbuf
            s.stride = 0
            try:
                s.color_space = lc.ColorSpace.Sycc()
            except Exception:
                pass
        status = cfg.validate()
        if status == lc.CameraConfiguration.Status.Invalid:
            send(f"ERROR configuration not accepted ({mw}x{mh} + {lw}x{lh})")
            return
        if status == lc.CameraConfiguration.Status.Adjusted:
            log("Camera configuration was adjusted: " + " ".join(str(cfg.at(i)) for i in range(cfg.size)))
        self.cam.configure(cfg)
        self.cfg = cfg
        t1 = time.monotonic()
        streams = [cfg.at(i).stream for i in range(cfg.size)]
        self.alloc = lc.FrameBufferAllocator(self.cam)
        bufs = []
        for s in streams:
            self.alloc.allocate(s)
            bufs.append(self.alloc.buffers(s))
        n = min(len(b) for b in bufs)
        desc = []
        for i in range(cfg.size):
            c = cfg.at(i)
            desc += [c.size.width, c.size.height, c.stride, c.frame_size]
        if len(desc) == 4:
            desc += [0, 0, 0, 0]
        send("CONFIGURED " + " ".join(str(x) for x in desc) + f" {n}")
        for k in range(n):
            req = self.cam.create_request(k)
            fds, parts = [], []
            for si, s in enumerate(streams):
                fb = bufs[si][k]
                req.add_buffer(s, fb)
                parts.append(str(len(fb.planes)))
                for p in fb.planes:
                    fds.append(p.fd)
                    parts += [str(p.offset), str(p.length)]
            self.reqs.append(req)
            self.where.append("idle")
            send(f"REQ {k} {len(streams)} " + " ".join(parts), fds)
        t2 = time.monotonic()
        self.start()
        if not self.timed:                       # (the first set-up: how long each part took)
            self.timed = time.monotonic()
            log(f"set up in {self.timed - t0:.2f} s (configure {t1 - t0:.2f}, buffers {t2 - t1:.2f}, "
                f"start {self.timed - t2:.2f})")

    def start(self):
        if self.cfg is None or self.running:
            return
        lc = self.lc
        us = int(1e6 / self.fps)
        ctl = {lc.controls.FrameDurationLimits: self.fdl or (us, us)}
        for name, v in list(self.sticky.items()) + list(self.controls.items()):
            try:
                ctl[getattr(lc.controls, name)] = v
            except AttributeError:
                log(f"control {name} not known")
        self.cam.start(ctl)
        self.running = True
        for i, w in enumerate(self.where):
            if w == "idle":
                self.queue(i)

    def queue(self, i):
        r = self.reqs[i]
        r.reuse()                                # (a used or cancelled request: pending again)
        if self.controls:
            for name, v in self.controls.items():
                try:
                    r.set_control(getattr(self.lc.controls, name), v)
                except Exception as e:
                    log(f"control {name} not set: {e}")
            self.controls_sent += 1
            if self.controls_sent >= len(self.reqs):
                self.controls = {}
        self.cam.queue_request(r)
        self.where[i] = "queued"

    controls_sent = 0
    timed = 0.0
    ev_restarts = 0

    def set_ev(self, ev):
        """(0.1.0) EV: applied by restarting the camera (about 0.05 s), not
        with the next pictures. The Raspberry Pi camera library holds settings
        like EV back until "their" picture comes round, counting pictures from
        each start - but it keeps the ones still waiting at a stop, so after
        the camera's first restart (a new mode, rotation, screen ...) every EV
        sent with the pictures waited behind those for minutes: the picture
        only changed at the next restart. A start's own settings are used at
        once."""
        ev = float(ev)
        old = self.sticky.get("ExposureValue", 0.0)      # (0: the camera's own default)
        self.sticky["ExposureValue"] = ev
        if ev == old or not self.running:
            return                               # (not running: used at the next start)
        t = time.monotonic()
        self.stop()
        self.start()
        self.ev_restarts += 1
        if self.ev_restarts == 1:
            log(f"EV {ev:+.1f}: camera restarted in {time.monotonic() - t:.2f} s to use it")

    # -- frames ------------------------------------------------------------
    def drain(self):
        lc = self.lc
        for r in self.cm.get_ready_requests():
            k = r.cookie
            if k >= len(self.reqs) or self.where[k] != "queued":
                continue                          # (from an earlier set-up)
            if r.status == lc.Request.Status.Cancelled or not self.running:
                self.where[k] = "idle"
                continue
            self.frames += 1
            if self.frames % 8 == 1:
                try:
                    md = {k2.name: v for k2, v in r.metadata.items()}
                    self.meta = md
                except Exception:
                    pass
            md = self.meta
            ts = 0
            for _s, fb in r.buffers.items():
                ts = fb.metadata.timestamp // 1000
                break
            self.where[k] = "client"
            if self.frames == 1 and self.timed:
                log(f"first picture {time.monotonic() - self.timed:.2f} s after starting")
            send(f"FRAME {k} {r.sequence} {ts} {md.get('ExposureTime', 0)} "
                 f"{md.get('AnalogueGain', 0):.2f} {md.get('Lux', 0):.0f} {md.get('ColourTemperature', 0)}")

    def command(self, text):
        p = text.split()
        if not p:
            return True
        c = p[0]
        if c == "DONE":
            k = int(p[1])
            if 0 <= k < len(self.reqs) and self.where[k] == "client":
                self.where[k] = "idle"
                if self.running:
                    self.queue(k)
        elif c == "CONF":
            mw, mh, lw, lh, hf, vf, nb = (int(x) for x in p[1:8])
            self.configure(mw, mh, lw, lh, hf, vf, nb, float(p[8]))
        elif c == "CTRL":
            name = p[1]
            vals = [float(x) if "." in x else int(x) for x in p[2:]]
            if name == "ExposureValue":
                self.set_ev(vals[0])
                return True
            self.controls[name] = tuple(vals) if len(vals) > 1 else vals[0]
            self.controls_sent = 0
            if name != "FrameDurationLimits":
                self.sticky[name] = self.controls[name]
            if name == "FrameDurationLimits":
                self.fps = 1e6 / vals[0]
                self.fdl = (int(vals[0]), int(vals[-1]))
        elif c == "STOP":
            self.stop()
            send("STOPPED")
        elif c == "START":
            self.start()
        elif c == "QUIT":
            self.stop()
            return False
        return True

    def loop(self):
        poll = select.poll()
        poll.register(SOCK.fileno(), select.POLLIN)
        poll.register(self.cm.event_fd, select.POLLIN)
        while True:
            for fd, ev in poll.poll(1000):
                if fd == self.cm.event_fd:
                    self.drain()
                elif ev & (select.POLLHUP | select.POLLERR):
                    return
                else:
                    try:
                        data = SOCK.recv(4096)
                    except OSError:
                        return
                    if not data:
                        return
                    if not self.command(data.decode(errors="replace")):
                        return


def main():
    # (the camera comes first: started by the fast start, loading the library
    # is the slowest part of the start - Zero Ichi itself waits with a lower
    # priority meanwhile; the start-up picture program mostly waits for the
    # screen, so it isn't held up)
    try:
        os.setpriority(os.PRIO_PROCESS, 0, -2)
    except OSError:
        pass
    t = time.monotonic()
    try:
        import libcamera as lc
    except Exception as e:
        send(f"ERROR camera library not found ({e})")
        await_client(20)
        return
    try:
        os.setpriority(os.PRIO_PROCESS, 0, -2)       # (from now on the camera comes first)
    except OSError:
        pass
    log(f"camera library loaded in {time.monotonic() - t:.2f} s")
    bootmark("camera library loaded")
    srv = Server(lc)
    if not srv.open():
        await_client()                           # (Zero Ichi hears why)
        return
    await_client()
    try:
        srv.loop()
    finally:
        try:
            srv.stop()
            srv.cam.release()
        except Exception:
            pass


def run():
    try:
        main()
    except Exception:
        import traceback
        send("ERROR " + " | ".join(traceback.format_exc().strip().splitlines()[-3:]))
        await_client(20)
        sys.exit(1)


if __name__ == "__main__":                       # (the fast start: camd_start.py - run())
    run()
