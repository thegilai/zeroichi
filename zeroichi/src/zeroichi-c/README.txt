Zero Ichi's C program (zeroichi-c) - the source
=============================================
Zero Ichi runs as this small C program; it makes the same pictures as the
Python Zero Ichi (app.py and friends), which stays as the backup.

  app_main.c     start-up, main loop, buttons, menu, power, standby
  app_view.c     picture sizes, camera set-up, live view, everything on top
  app_media.c    photos, GIFs, videos (recording, saving, developing)
  app_gallery.c  the gallery          app_space.c   Photo space, card image
  app_presets.c  MENU > Presets
  looks.c        the looks (a port of filters.py / pixelart.py)
  img.c          picture operations, made to match Pillow exactly
  tables.c       colour tables and palettes (made by gen/gen_tables.py - the
                 Zero Ichi film's from zifilm.py, made by gen/gen_zifilm.py)
  fontdata.c     the letters (DejaVu Sans Bold, drawn by Pillow; gen/gen_fonts.py);
                 fontbrand.c: the menus' letters (Poppins; gen/gen_brandfont.py)
  screensaver.c  the screen saver (v51) and the start-up picture's fallback;
                 brandart.c: its two pictures, made with the start-up pictures
                 (splash*.rgb565) and brandart.py by gen/gen_brand.py
  camera.c       talks to camd.py, which runs the camera with libcamera and
                 hands over its frame buffers (no copies)
  venc.c         videos with the Pi's hardware encoder; jpeg.c: JPEG (libjpeg-
                 turbo + the hardware encoder); mov.c: .mov files; gif.c: GIFs
  lcd.c, gpio.c  the screen (SPI) and buttons; state.c: settings.txt, state.json
                 and the presets (all in the zeroichi folder on the SD card)
  util.c         small helpers - and sys_write: the system partition is
                 read-only while Zero Ichi runs, opened only for a moment
Build: see build.sh. Pillow's colour tables in pillow_ycc.h: see LICENSE-Pillow.
