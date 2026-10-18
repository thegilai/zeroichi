# Changelog

## 0.1.0 — first public release

Zero Ichi turns a Raspberry Pi Zero, an OV5647 camera and a small LCD hat
with a joystick and three keys into a pocket toy camera.

**Shooting**
- PHOTO, GIF and VIDEO modes. Photos are 1296×972 (4:3) or 972×972 (1:1)
  and are taken straight from the live picture, so there's no shutter lag.
- GIFs and videos are 320×240 or 640×480, at 15 frames per second. Videos are
  saved the moment you stop, with every look.
- Live preview of every look at up to 30 pictures a second.
- Brightness (EV −2 … +2), shown live as you change it.
- Rotation, mirror and image ratio (Fit screen / 4:3 / 1:1). Changing them in
  the menu shows the live picture for a moment.

**17 looks**
- Normal
- Film: Super 8, Holga B&W, Holga Color, Zero Ichi
- Black & white: Mono, Hi-Con B&W
- Game Boy Camera style: GB Camera, GB Camera B&W
- Pixel art: 16-Bit Arcade, 16-Bit Neon, 16-Bit Paper
- Colour effects: Cross, Thermal
- Prisms: Prism, Prism 3, Prism Rainbow

**Camera**
- Starts in about 5 seconds: a fast start skips the normal Linux start-up,
  and a small C program does the camera work.
- Safe to unplug whenever the red dot isn't showing, because the card is
  read-only between saves.
- Uses the whole SD card for photos (set up automatically, once).
- 4 presets, kept as editable text files.
- Erase all: deletes every photo, GIF and video in one go. It asks twice and
  keeps your settings and presets.
- Offline by design: Wi-Fi and Bluetooth are switched off in the Pi's
  firmware on a Zero W / Zero 2 W. The Pi Zero has neither.
- Gallery with GIF and video playback.
- Screen saver and Standby (wakes in about a second).
- Works with the 1.3inch 240×240 and 2.8inch 320×240 screens. Each camera
  remembers its own screen.
- Can make a ready-to-flash card image of itself, trimmed to be as small as
  possible.
- Photos, GIFs and videos record the look they were made with (e.g. "Look:
  Super 8") and the program that made them, with no date or location.
