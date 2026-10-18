ZERO ICHI 0.1.0 - a toy camera for the Raspberry Pi Zero
=========================================================
by thegilai

Zero Ichi (ゼロイチ, "zero one") turns a Raspberry Pi Zero, a small camera
module and a tiny screen with buttons into a pocket toy camera: 17 looks
(film, Holga, Game Boy-style, pixel art, prisms...), photos, animated GIFs
and videos, a live preview of every look, and a start-up of about 5 seconds.
It is offline by design, like a film camera: no Wi-Fi, no Bluetooth, no
apps - your pictures leave the camera only on its SD card.

  1  What you need                  11  Screens
  2  Installing                     12  Your data
  3  The buttons                    13  Photo space (using the whole card)
  4  The menu                       14  Start-up and power
  5  Looks                          15  Making a card image
  6  Photos, GIFs and videos        16  Updating
  7  Getting your files             17  settings.txt
  8  Safe to unplug                 18  Troubleshooting
  9  Presets                        19  Building from source
  10 Card space                     20  Credits and licence


1  WHAT YOU NEED
  - Raspberry Pi Zero (made and tested on a Pi Zero v1.3 - the Zero W and
    Zero 2 W have the same camera connector and should work too)
  - an OV5647 camera module (Raspberry Pi Camera v1.3 or a compatible
    "5MP OV5647" module) and a Pi Zero camera cable
  - a screen with a joystick and 3 keys:
      Waveshare 1.3inch LCD HAT (240x240), or
      a 2.8inch 320x240 display hat with the same pins and buttons
    (other 320x240 SPI screens work too - see 17 settings.txt)
  - a microSD card, 8 GB or more (16-32 GB recommended)
  - a 5 V power supply (2.5 A) or a battery pack
  - for installing: a computer with an SD card reader
  No Wi-Fi or Bluetooth is used, ever. On a Pi Zero W or Zero 2 W, Zero Ichi
  switches both off in the Pi's firmware at the first start (config.txt,
  "# Zero Ichi: no Wi-Fi, no Bluetooth"), so they don't exist for the
  system at all. (The Pi Zero has neither.)


2  INSTALLING
  There are two ways. The easiest is a ready-made card image, if one comes
  with this release: flash it and switch on (see 15 for what it does at the
  first start). Otherwise install Zero Ichi on a fresh Raspberry Pi OS card:

  1. Flash "Raspberry Pi OS Lite (32-bit)" onto the card with BalenaEtcher.
     (With Raspberry Pi Imager: don't use its "OS customisation" settings -
     they replace this kit's first-start setup.)
  2. Take the card out and put it back in. Copy EVERYTHING from this kit onto
     the "bootfs" drive, and choose REPLACE when asked.
     (The file "user-data" sets up the Pi at its first start, including a
     user "pi" with the password "zeroichi" - only for logging in with a
     keyboard and monitor if you ever need to; there's no network to log in
     from. Change the password there with a text editor if you like.)
  3. Zero Ichi needs 3 software packages that aren't in Raspberry Pi OS
     Lite. The Pi Zero has no internet, so your computer downloads them onto
     the card and the Pi installs them:
     a. With the card in your computer, run the downloader:
          Windows: double-click GET-PACKAGES.bat on the bootfs drive.
          Mac:     open Terminal and type:  sh /Volumes/bootfs/GET-PACKAGES-MAC.command
          Linux:   sh GET-PACKAGES-MAC.command (in the card's bootfs folder)
        It downloads the package lists (about 40 MB).
     b. Eject the card, put it in the Pi and switch on. After a few minutes
        the screen says "PC STEP NEEDED" and the Pi switches itself off.
     c. Wait until the green LED is off. Put the card in your computer and
        run the downloader again: this time it downloads the packages.
     d. Put the card back in the Pi and switch on. The screen says
        "INSTALLING" for 10-20 minutes - don't unplug it. Then Zero Ichi
        starts.
     If the Pi asks for the computer step again, repeat c and d - it only
     asks for what's still missing. The progress is in offline\STATUS.txt.
  4. At the next start Zero Ichi sets the card up so the WHOLE card holds
     photos (see 13) - about 15 minutes, once. Leave it switched on.
     Afterwards the card shows one drive on your computer, ZEROICHI.
  5. The first start uses the 240x240 screen. With the 2.8inch screen: hold
     KEY2 while switching on until the start-up picture shows, or choose
     MENU > Screen > 320x240 (see 11).


3  THE BUTTONS (KEY1 = top button, KEY3 = bottom button)
  Camera:
    KEY1                 take a photo / start + stop recording
    KEY3                 switch mode: PHOTO -> GIF -> VIDEO
    Joystick left/right  change the look (also while recording)
    Joystick up/down     brightness (EV -2 ... +2)
    Joystick press       open the MENU
    KEY2                 gallery
    Hold KEY3 for 3 s    power off (or Standby - MENU > KEY3 hold)
  Gallery:
    left/right = browse, press = play a video, KEY3 = delete, KEY2 = back.
    GIFs play by themselves.


4  THE MENU (press the joystick)
  Up/down = choose a row, left/right = change it, KEY2 (or press) = close.
  Everything you set is saved at once and kept after switching off.
    Presets      Press: save / load your settings (see 9)
    Rotation     0 / 90 / 180 / 270 degrees - turns the camera picture
                 (it keeps the image ratio: never portrait)
    Screen       320x240 (2.8inch screen) or 240x240 (1.3inch HAT) -
                 switches right away and is remembered (see 11)
    Flip screen  Off / On - the screen turned 180 degrees
    Mirror       Off / On - the picture mirrored
    Image ratio  Fit screen (the screen's own shape: 4:3 on the 2.8inch
                 screen, 1:1 on the 1.3inch HAT), 4:3 or 1:1.
                 Photos: 1296x972 (4:3) or 972x972 (1:1).
    Clip size    GIF and video size: 320x240 or 640x480 (1:1 pictures:
                 240x240 / 480x480)
    GIF frames   8 / 16 / 24
    Video max    15 s / 30 s / 1 min / 2 min / 5 min / 10 min / 12 min - a
                 recording stops by itself then (or with KEY1, or when the
                 card is full)
    Screen saver Off / 30 s / 1 min / 2 min / 5 min without a key press:
                 Zero asleep, with shapes drifting by; the camera pauses to
                 save power, and after 10 minutes the screen goes dark. Any
                 key wakes it up (that key press does nothing else). Never
                 while recording or saving.
    Photo space  Use the whole card for photos (see 13) - only shown while
                 the card doesn't use all its room yet
    Erase all    Press: erase every photo, GIF and video on the card (it
                 asks twice: KEY3 = yes, any other key = no). Your settings,
                 presets and Zero Ichi itself stay, and new files keep
                 counting up from the last number. It can't be undone - copy
                 your files to a computer first (see 7).
    KEY3 hold    Power off, or Standby: holding KEY3 switches the screen and
                 camera off and slows the Pi down; any key wakes it up in
                 about a second, ready to shoot. Standby still uses some
                 power - for long breaks, power off (MENU > Power off).
    Power off    Press: switch off (same as holding KEY3)
    About        Press: the version, the screen, the start-up time, what's
                 on the card. Any key: back
  Changing Rotation, Mirror or Image ratio shows the live picture for 3
  seconds with the setting in a small card, so you see what changes; up,
  down or press goes straight back to the menu.


5  LOOKS
  Joystick left/right goes through them (from Normal, left goes straight to
  Zero Ichi). The screen shows each look live, and photos, GIFs and videos
  come out the same.
    Normal         the camera's own picture
    Super 8        home movie film: warm, rich colours, golden highlights,
                   grain, rounded film-gate corners; in GIFs and
                   videos it moves like film in a projector (breathing
                   brightness, a slight drift, now and then an orange light
                   leak). Photos: about 2 in 5 get a light leak.
    Holga B&W      plastic-lens film camera: square 6x6, sharp only in the
                   middle, soft edges, dark corners, black & white with grain
    Holga Color    the same lens on colour film: punchy contrast, strong,
                   slightly warm colours, grain. Square 6x6.
    GB Camera      Game Boy Camera style: a 128-pixel grid, 4 shades of
                   green with its typical dithering
    GB Camera B&W  the same in 4 shades of grey
    16-Bit Arcade, 16-Bit Neon, 16-Bit Paper
                   pixel art: a chunky 160x120 grid, a small palette and a
                   fine dither (Arcade: bright retro game colours, Neon:
                   pinks, purples and cyans, Paper: soft printed-zine colours)
    Mono           black & white
    Hi-Con B&W     hard black and white, inspired by Daido Moriyama's street
                   photos: crushed blacks, burnt-out whites, sharp
    Cross          cross-processed film colours
    Thermal        a heat camera's colours
    Prism          a kaleidoscope prism filter: a clear middle and 6 facets
                   repeating your subject around itself, rainbow edges
    Prism 3        3 bigger copies around the middle
    Prism Rainbow  the 6 facets with much stronger rainbow fringes
    Zero Ichi      the camera's own colour film, after its logo: rich reds,
                   golden yellows, cobalt blues, inky shadows, creamy
                   highlights, fine grain
  The black & white looks (Holga B&W, GB Camera, GB Camera B&W, Mono,
  Hi-Con B&W, Thermal) only use the brightness: they are the quickest.


6  PHOTOS, GIFS AND VIDEOS
  PHOTO: KEY1 takes the photo straight from the running camera picture -
    exactly the exposure you see, no delay. It is finished and saved in the
    background ("SAVING" in the corner), so you can keep shooting.
  GIF: KEY1 records 8, 16 or 24 frames (MENU > GIF frames) at the clip
    size; it is saved as an animated GIF.
  VIDEO: KEY1 starts and stops. 15 frames per second at the clip size,
    saved the moment you stop - nothing to wait for. With Normal the Pi's
    hardware video encoder records it; with any other look the video is the
    picture the screen shows (without the on-screen text), enlarged smoothly
    to the clip size. With a look, 320x240 has the same detail as 640x480
    in about a third of the file size.
    The screen runs at up to 30 pictures a second (in VIDEO mode too, when
    the look and the light allow it); videos are always 15.
    The heaviest looks (the Prisms) may not keep a full 15 frames per
    second at 640x480 - 320x240 is lighter.
  The bottom left shows the mode and size, e.g. "VIDEO 640x480" or
  "PHOTO 1:1"; the bottom right the number of files.
  Files: IMG_0001.jpg, GIF_0002.gif, VID_0003.mov (plays in QuickTime /
  VLC). The number counts up across all three and is never used twice, even
  after deleting, so copying to your computer never overwrites anything.
  Each file says what made it and with which look: photos have camera maker
  "Zero Ichi", model "Zero Ichi cam", the program (Zero Ichi cam, its
  version and who made it) and the look as their description, e.g. "Look:
  Super 8" (see the file's info / properties on a computer); videos the
  same, with the look as their comment; GIFs all of it in their comment. No
  date: the Pi has no clock of its own (the file date on your computer is
  the time the Pi carried on from).


7  GETTING YOUR FILES
  Switch off (hold KEY3 and wait for the green LED to go off) - or just
  unplug when there's no red dot (see 8). Put the card in your computer and
  open the "photos" folder on the ZEROICHI drive (called bootfs before the
  card was set up; small cards: a separate PHOTOS drive).
  To empty the card afterwards: MENU > Erase all. (Don't format the card's
  drive on a computer: it also holds Zero Ichi and your settings.)
  If your computer ever asks to format a drive on this card, click Cancel -
  that's the Pi's system partition.


8  SAFE TO UNPLUG
  While the camera is on, Zero Ichi keeps the whole card read-only whenever
  nothing is being saved, so pulling the plug can't damage it - like taking
  the batteries out of a film camera. A small RED DOT in the top right
  corner shows while the card is being written:
    - while a photo or GIF is being saved,
    - the whole time a video records, and while it's saved,
    - for about 2 seconds after you change a setting or delete something,
    - for a moment when the camera picture first shows after start-up.
  Red dot = don't unplug. No red dot = unplug any time.
  - Unplugging while recording loses that video.
  - Never unplug during the one-time jobs that say "Don't switch off"
    (installing, Photo space, making a card image, updating).
  - KEY3 is still the tidiest way: it finishes any saving first and shows
    "Switched off".


9  PRESETS
  A preset keeps every setting: the look, mode, brightness and every MENU
  setting. There are 4.
  - Save: set the camera up, then MENU > Presets > press. Choose a number
    with up/down and press KEY2 (a used one asks first: KEY2 again
    replaces it).
  - Load: MENU > Presets > press, choose the number, KEY1.
  Each preset is a small text file on the card, zeroichi\presets\preset-1.txt
  to preset-4.txt - you can read and edit them on a computer (e.g. "look =
  GB Camera", "ev = +0.5", "image_ratio = 1:1"), copy them to another Zero
  Ichi's card, or delete one to empty it. A preset only sets the screen on
  the Zero Ichi it was saved on (see 11).


10 CARD SPACE
  The bottom left of the camera screen shows how much still fits: "1234
  photos left", "56 GIFs left" or "1h05m left" (of video), for the look and
  size you've chosen - more exact as you shoot. It turns red below 200 MB.
  Below 30 MB Zero Ichi says "Card full!", and a recording stops by itself
  (and is kept) when the card fills up.


11 SCREENS
  Both screens use the same pins and buttons. These screens can't tell the
  Pi what they are, so each Zero Ichi remembers its own screen (by the Pi's
  serial number): a card can go back and forth between two Zero Ichi
  cameras with different screens, and each starts with the right one.
  - A new card (or a Zero Ichi the card hasn't met yet) starts with the
    screen used last - a new card: 240x240.
  - Wrong screen (garbled picture)? Hold KEY2 while switching on until the
    start-up picture shows, or choose MENU > Screen. From then on that Zero
    Ichi always starts with the right screen.
  - Other 320x240 screens (ST7789 or ILI9341): "display" in settings.txt.


12 YOUR DATA
  Everything of yours is on the card's ZEROICHI drive, where you can see it:
    photos\                     your photos, GIFs and videos
    zeroichi\settings.txt       start-up options (see 17)
    zeroichi\state.json         everything set in the camera and its MENU
    zeroichi\presets\           your presets
    zeroichi\zeroichi-log.txt   what happened (for troubleshooting)
    zeroichi\boot-time.txt      what the start-up spent its time on
  The card's other part (the Pi's system) holds only software. Put the card
  in another Zero Ichi and everything comes with it; copy the ZEROICHI drive
  to keep a backup.


13 PHOTO SPACE (using the whole card)
  A fresh Raspberry Pi OS card keeps its files on the small bootfs drive
  (512 MB). At the first start after installing, Zero Ichi shows "Use the
  whole card for photos" with a 10 second countdown, then, once:
  - makes the Pi's system partition smaller (4-5 GB) and moves it to the end
    of the card,
  - turns the rest of the card into ONE big drive, ZEROICHI, and copies the
    start-up files, Zero Ichi and your files onto it (every copy is checked
    first, so the card still starts at every moment).
  It takes about 15 minutes; the screen shows each step. DON'T switch off
  meanwhile. On a 32 GB card that's about 26 GB for photos and videos.
  - KEY2 during the countdown = not now (it asks at the next start); KEY1 =
    start right away. "photo_space = menu" in settings.txt: only when you
    choose MENU > Photo space.
  - 8 GB cards get a separate PHOTOS drive instead (no room to move the
    system), so they show two drives.
  - If something goes wrong, Zero Ichi says "didn't work" and tries again at
    the next start (twice at most). Details: "Make room for photos" in
    zeroichi-log.txt.


14 START-UP AND POWER
  Zero Ichi starts in about 5 seconds: the Pi skips the normal Linux
  start-up and starts straight into Zero Ichi (the "fast start"), and the
  camera system loads while the screen shows the start-up picture. The
  camera picture appears when it's ready.
  - The first start of a new install or version may show "One-time setup":
    Zero Ichi sets the Pi up to start faster (from the next start).
  - Power off: hold KEY3 for 3 seconds, wait until the green LED is off. Or
    unplug when there's no red dot (see 8).
  - For a quicker "on", use Standby (MENU > KEY3 hold): it wakes in about a
    second.
  - The normal Linux start-up is still there: hold KEY1 while switching on
    (just that once), or "fast_start = no" in settings.txt. If Zero Ichi
    keeps failing to start, the Pi switches to the normal start-up by itself.
  - If the Pi ever doesn't start: put the card in your computer, open
    cmdline.txt on the ZEROICHI (or bootfs) drive and delete
    " init=/usr/local/sbin/zeroichi-init" at the end of the line (keep the
    rest on one line). If it still doesn't start, open config.txt and delete
    the lines from "# Zero Ichi fast boot" to "# Zero Ichi fast boot end".
  - To undo all start-up changes: "optimize_boot = no" in settings.txt, and
    restart twice.


15 MAKING A CARD IMAGE (to set up more Zero Ichi cameras)
  A finished Zero Ichi card (after Photo space) can make an image of itself
  to flash onto new cards:
  1. With the card in your computer, make an empty file called
     MAKE-IMAGE.txt inside the "zeroichi" folder.
  2. Put the card in the Pi and switch on. "Making a card image" shows each
     step and a progress bar - about 30-60 minutes; don't switch off. When
     it says "Card image ready!" the Pi tidies up and switches itself off:
     wait until the green LED is off.
  3. Copy zeroichi-card.img.xz from the card's drive (its first page, not in
     a folder) onto your computer, then delete it from the card.
  Flash it onto a new card (8 GB or more) with BalenaEtcher or Raspberry Pi
  Imager - no need to unpack it - and switch on: at the first start it
  stretches its ZEROICHI drive to the size of the card (a few seconds), and
  the camera is ready, with the 240x240 screen (see 11).
  The image has Zero Ichi and its settings.txt, but not your photos, presets,
  logs or camera settings. It leaves out what a camera never needs, to be
  as small as possible: download caches, old logs, the swap file, the start
  files of Pi 4, 400 and 5, manuals, docs and translations (the licences
  stay), the firmware of Wi-Fi and Bluetooth chips and the Pi 4 / 5 boot
  EEPROM images. (They are left out of the card it's made from too - the
  camera works the same.) ZEROICHI can use up to 128 GB of a card.
  Smaller still: the Pi packs the image quickly (xz -1). To publish it,
  pack it again on a computer at the strongest setting - Mac/Linux:
    xz -dc zeroichi-card.img.xz | xz -9e -T0 > zeroichi-card-0.1.0.img.xz
  Windows: 7-Zip - extract zeroichi-card.img, then Add to archive, format
  xz, level Ultra. It flashes just the same.


16 UPDATING
  Open the new kit's "zeroichi" folder, select ALL the files inside it, and
  drag them into the "zeroichi" folder on the card -> choose "Replace".
  Don't drag the folder itself onto the card: on a Mac, "Keep Both" makes a
  second folder the Pi never uses, and "Replace" deletes your settings and
  presets. Zero Ichi sees the new files at the next start and updates
  itself (that start takes a few seconds longer, once). Check it worked:
  the start-up picture and MENU > About show the version (e.g. "v0.1.0").


17 SETTINGS.TXT
  zeroichi\settings.txt has a few start-up options - edit it with a plain
  text editor (TextEdit on a Mac: Format > Make Plain Text). Each option is
  explained in the file: where photos go, Photo space, the power button, the
  fast start, the screen type and its colours, pins and speed. Changes apply
  at the next start.


18 TROUBLESHOOTING
  - Screen garbled, or wrong colours: hold KEY2 while switching on (the
    other screen size), or try "display_bits = 18" in settings.txt.
  - "No camera found - still trying": check the ribbon cable (both ends,
    contacts the right way). Zero Ichi switches to the camera by itself as
    soon as it appears.
  - Slow or jerky: zeroichi-log.txt has a "Live view" line every 30 seconds
    (pictures per second, where the time went, the Pi's temperature and
    speed - "under-voltage" means the power supply or cable is too weak),
    and a line for every photo, GIF and video. boot-time.txt shows the
    start-up step by step.
  - The quick Zero Ichi is a small C program; the original Python Zero Ichi
    stays on the card as a backup and takes over by itself if the C program
    can't run ("app = python" in settings.txt uses it on purpose).
  - For a full system report: "diagnostics = yes" in settings.txt (makes the
    start slower) - it writes zeroichi-diagnostics.txt.
  When asking for help, send zeroichi-log.txt and boot-time.txt.


19 BUILDING FROM SOURCE
  Everything is in this kit: the Python Zero Ichi and the camera helper
  (zeroichi\*.py), the start-up script (zeroichi\fast-start.sh), and the C
  sources in zeroichi\src:
    src\zeroichi-c\   the quick Zero Ichi (zeroichi-c) - see its README.txt
    src\zeroichi-lcd.c   the start-up picture / small start-up jobs
  zeroichi-c and zeroichi-lcd are built for the Pi Zero's processor (ARMv6,
  static) with zig 0.17 as the cross compiler and libjpeg-turbo 3.1 - see
  src\zeroichi-c\build.sh. On the Pi itself: gcc -O2 -o zeroichi-c *.c
  -ljpeg -lm -lpthread (slow, but works).


20 CREDITS AND LICENCE
  Zero Ichi is designed by thegilai; the code was written with the help of
  Claude (Anthropic's AI assistant). It started from the SeedSigner project
  (seedsigner.com): the same Pi Zero, camera and screen hat, turned into a
  camera - thanks to the SeedSigner team (Zero Ichi is a separate project,
  not affiliated with SeedSigner). Licence: see LICENSE (MIT).
  Checking a download: see "Verify your download" in README.md (SHA-256).
  It includes work by others - libjpeg-turbo, musl, Pillow's colour
  conversions and resampling, the Poppins and DejaVu fonts: see
  THIRD-PARTY-NOTICES.txt. This software is based in part on the work of
  the Independent JPEG Group.
  Holga, Game Boy and Kodachrome are trademarks of their owners; the looks
  are inspired by them, and Zero Ichi is not affiliated with them.
  Changes from version to version: CHANGELOG.md.
