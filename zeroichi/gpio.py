"""
Minimal GPIO access for Zero Ichi.

Uses the small lgpio library directly - gpiozero is much slower to load on a
Pi Zero (it imports a lot and probes several pin libraries at start-up).
Falls back to gpiozero if lgpio isn't available.
"""
import logging
import os
import threading
import time

os.environ.setdefault("LG_WD", os.getcwd())   # lgpio writes helper files here

log = logging.getLogger("zeroichi.gpio")

_lg = None
_chip = None
_lock = threading.Lock()


def _open():
    """Open the main GPIO chip once (the one with the 40-pin header)."""
    global _lg, _chip
    if _chip is not None:
        return _lg, _chip
    import lgpio
    _lg = lgpio
    best = None
    for n in range(6):
        try:
            h = lgpio.gpiochip_open(n)
        except Exception:
            continue
        try:
            _, lines, _, label = lgpio.gpio_get_chip_info(h)
        except Exception:
            lines, label = 0, ""
        if best is None and ("pinctrl" in label or lines >= 54):
            best = h
        else:
            lgpio.gpiochip_close(h)
    if best is None:
        best = lgpio.gpiochip_open(0)
    _chip = best
    return _lg, _chip


class OutputPin:
    def __init__(self, pin, value=0):
        self.pin = pin
        try:
            self.lg, self.h = _open()
            self.lg.gpio_claim_output(self.h, pin, value)
            self.dev = None
        except Exception as e:
            log.info("lgpio not usable (%s) - falling back to gpiozero", e)
            from gpiozero import DigitalOutputDevice
            self.lg = None
            self.dev = DigitalOutputDevice(pin, initial_value=bool(value))

    def on(self):
        if self.dev is None:
            self.lg.gpio_write(self.h, self.pin, 1)
        else:
            self.dev.on()

    def off(self):
        if self.dev is None:
            self.lg.gpio_write(self.h, self.pin, 0)
        else:
            self.dev.off()

    def close(self):
        try:
            if self.dev is None:
                self.lg.gpio_free(self.h, self.pin)
            else:
                self.dev.close()
        except Exception:
            pass


def held_down(pin, checks=3):
    """Is the button on `pin` (to GND) held down right now? Used at start-up."""
    try:
        lg, h = _open()
        lg.gpio_claim_input(h, pin, lg.SET_PULL_UP)
        try:
            time.sleep(0.005)
            for _ in range(checks):
                if lg.gpio_read(h, pin) != 0:
                    return False
                time.sleep(0.01)
            return True
        finally:
            lg.gpio_free(h, pin)
    except Exception:
        return False


class ButtonPoller:
    """Watches buttons (pin to GND, internal pull-up) in a light background
    thread and calls on_event(name, "press" | "hold").

    hold_names: buttons that report a long hold. For those, a short press is
    reported when the button is released (so a hold never also counts as a press).
    """

    def __init__(self, pins, on_event, hold_names=(), hold_time=3.0, interval=0.02):
        self.pins = dict(pins)                 # name -> BCM pin
        self.on_event = on_event
        self.hold_names = set(hold_names)
        self.hold_time = hold_time
        self.interval = interval
        self._stop = threading.Event()
        self._order = list(self.pins)          # bit i of a reading = button _order[i]
        self.group = None
        self.dev = None
        try:
            self.lg, self.h = _open()
            try:                               # all buttons as one group: one read for all
                self.lg.group_claim_input(self.h, [self.pins[n] for n in self._order],
                                          self.lg.SET_PULL_UP)
                self.group = self.pins[self._order[0]]
            except Exception as e:
                log.info("Reading the buttons one by one (%s)", e)
                for pin in self.pins.values():
                    self.lg.gpio_claim_input(self.h, pin, self.lg.SET_PULL_UP)
            self._thread = threading.Thread(target=self._loop, daemon=True, name="buttons")
            self._thread.start()
        except Exception as e:
            log.info("lgpio not usable for buttons (%s) - falling back to gpiozero", e)
            self._gpiozero()

    # -- lgpio polling -----------------------------------------------------
    def _read(self):
        """The buttons held down right now, as bits (bit i = _order[i])."""
        lg, h = self.lg, self.h
        if self.group is not None:
            _, levels = lg.group_read(h, self.group)
            return ~levels & ((1 << len(self._order)) - 1)     # pressed = pulled low
        bits = 0
        for i, name in enumerate(self._order):
            try:
                if lg.gpio_read(h, self.pins[name]) == 0:
                    bits |= 1 << i
            except Exception:
                pass
        return bits

    def _loop(self):
        order = self._order
        hold_bits = {name: 1 << i for i, name in enumerate(order) if name in self.hold_names}
        any_hold = sum(hold_bits.values())
        try:
            stable = self._read()
        except Exception:
            stable = 0
        # Buttons already held when Zero Ichi starts (e.g. KEY2 to switch screens)
        # count only after they've been released and pressed again.
        last = stable
        held = {n for n, b in hold_bits.items() if stable & b}
        down_at = {}
        while not self._stop.is_set():
            now = time.monotonic()
            try:
                cur = self._read()
            except Exception:
                cur = last
            if cur != last:                        # needs 2 equal reads (debounce)
                last = cur
            elif cur != stable:
                changed = cur ^ stable
                for i, name in enumerate(order):
                    bit = 1 << i
                    if not changed & bit:
                        continue
                    if cur & bit:
                        down_at[name] = now
                        if name not in self.hold_names:
                            self.on_event(name, "press")
                    elif name in self.hold_names:
                        if name in held:
                            held.discard(name)
                        else:
                            self.on_event(name, "press")
                stable = cur
            if stable & any_hold:                  # a long-press button is down: time it
                for name, bit in hold_bits.items():
                    if stable & bit and name not in held \
                            and now - down_at.get(name, now) >= self.hold_time:
                        held.add(name)
                        self.on_event(name, "hold")
            # (a plain sleep: Event.wait() runs several Python-level steps every
            #  time - noticeable at 50 checks a second on a Pi Zero)
            time.sleep(self.interval)

    # -- gpiozero fallback -----------------------------------------------
    def _gpiozero(self):
        from gpiozero import Button
        self.dev = []
        for name, pin in self.pins.items():
            if name in self.hold_names:
                b = Button(pin, pull_up=True, bounce_time=0.03, hold_time=self.hold_time)
                state = {"held": False}

                def held(n=name, s=state):
                    s["held"] = True
                    self.on_event(n, "hold")

                def released(n=name, s=state):
                    if s["held"]:
                        s["held"] = False
                    else:
                        self.on_event(n, "press")
                b.when_held, b.when_released = held, released
            else:
                b = Button(pin, pull_up=True, bounce_time=0.03)
                b.when_pressed = (lambda n=name: self.on_event(n, "press"))
            self.dev.append(b)

    def close(self):
        self._stop.set()
        if self.dev:
            for b in self.dev:
                b.close()
        elif self.group is not None:
            try:
                self.lg.group_free(self.h, self.group)
            except Exception:
                pass
        else:
            for pin in self.pins.values():
                try:
                    self.lg.gpio_free(self.h, pin)
                except Exception:
                    pass
