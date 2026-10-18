#!/bin/sh
# zeroichi-init 6 (v53)
#
# Zero Ichi fast start: the whole start-up of the Pi, made just for the camera.
#
# The Linux kernel runs this instead of the normal start-up (systemd), which
# starts dozens of services a camera never needs. Zero Ichi's one-time setup puts
# it in place (/usr/local/sbin/zeroichi-init) and adds "init=..." to cmdline.txt.
# It mounts the card, loads just the screen and camera drivers, registers the
# camera's devices (see register_devices) and starts Zero Ichi - nothing else
# runs. Zero Ichi's power-off comes back here.
#
# Normal start-up instead (for maintenance):
#   * hold KEY1 while switching on (until the start-up screen appears), or
#   * set "fast_start = no" in zeroichi/settings.txt (from the next start), or
#   * on a computer: delete " init=/usr/local/sbin/zeroichi-init" from cmdline.txt
# If anything goes wrong here, the Pi also falls back to the normal start-up.
#
# Zero Ichi itself is a small C program (zeroichi-c, with its camera helper
# camd.py); the Python Zero Ichi (zeroichi.py) is kept as the backup: it takes over
# when settings.txt says "app = python" or when the C program can't run.
#
# Nothing of yours is kept on the system partition: settings, presets, photos
# and logs are all in the zeroichi folder / photos folder of the card's ZEROICHI
# drive. While Zero Ichi (C) runs, the system partition is even read-only (see
# system_ro) - pulling the plug can't harm it. It is only written for updates
# and the one-time jobs below.
#
# Exit codes Zero Ichi uses to ask for things:
#   64 = power off, 65 = normal start-up next time (then restart),
#   66 = restart, 67 = make room for photos (see make_space),
#   68 = files changed on the SD card: refresh the copy (zeroichi.py --maintenance),
#   69 = use the Python Zero Ichi (for this start), 70 = make a card image (make_image);
#   anything else = Zero Ichi stopped: start it again.

PATH=/usr/sbin:/usr/bin:/sbin:/bin
HOME=/root
LANG=C.UTF-8
ZEROICHI_FASTINIT=1
ZEROICHI_INIT=6                              # (what this script can do: see make_space, make_image)
export PATH HOME LANG ZEROICHI_FASTINIT ZEROICHI_INIT

STATE=/var/lib/zeroichi
APP=/var/lib/zeroichi/app
SD=/boot/firmware/zeroichi
LOG=/run/zeroichi/fast-start.txt
KEY1=21
JOBS=""
LCD=/usr/local/sbin/zeroichi-lcd

normal_start() {
    # Hand the Pi over to the normal start-up (systemd) - stop what we started
    [ -n "${CAMD:-}" ] && kill "$CAMD" >/dev/null 2>&1
    udevadm control --exit >/dev/null 2>&1
    umount /media/photos >/dev/null 2>&1
    umount /boot/firmware >/dev/null 2>&1
    [ -x /lib/systemd/systemd ] && exec /lib/systemd/systemd
    exec /sbin/init
}

mark() {
    # "seconds since the kernel started  what happened" -> boot-time.txt
    # (" @N": the processor's idle seconds so far - boot-time.txt works out
    #  how busy it was between the steps)
    { read -r up idle < /proc/uptime; echo "$up $1 @$idle" >> "$LOG"; } 2>/dev/null
}

key1_held() {
    # KEY1 (pin to ground) held down? zeroichi-lcd looks (a few milliseconds) - it
    # did already, with the first mounts (v52: EARLY); without it the "pinctrl"
    # tool (slower); without either: no
    case "${EARLY:-}" in
        *" held") return 0 ;;
        *" free") return 1 ;;
    esac
    if [ -x "$LCD" ]; then
        "$LCD" --held "$KEY1" 2>/dev/null
        case $? in 0) return 0 ;; 1) return 1 ;; esac
    fi
    command -v pinctrl >/dev/null 2>&1 || return 1
    pinctrl set "$KEY1" ip pu >/dev/null 2>&1 || return 1
    n=0
    for _ in 1 2 3; do
        sleep 0.02
        case "$(pinctrl get "$KEY1" 2>/dev/null)" in
            *"| lo"*) n=$((n + 1)) ;;
        esac
    done
    [ "$n" -eq 3 ]
}

show() {
    # Put a picture Zero Ichi prepared ($1) back on the screen. (When Zero Ichi stops,
    # the screen's pins are let go and its last picture would vanish.)
    if [ -x "$LCD" ] && [ -f "$1" ] && [ -f "$STATE/lcd.conf" ]; then
        "$LCD" "$STATE/lcd.conf" "$1" >/dev/null 2>&1
    fi
    rm -f "$1" 2>/dev/null
}

message() {
    # Zero Ichi's last picture ("Switched off", "Making a card image..."): in RAM
    # (older Zero Ichi versions: on the system partition)
    if [ -f /run/zeroichi/message.rgb565 ]; then
        show /run/zeroichi/message.rgb565
    else
        show "$STATE/message.rgb565"
    fi
}

system_ro() {
    # While Zero Ichi (C) runs, the system partition is read-only: nothing is
    # written there in use, and pulling the plug can't harm it. (Zero Ichi opens
    # it for a moment when it has to: its sys_write.) If it can't be done,
    # Zero Ichi runs as before.
    sync
    mount -o remount,ro / >/dev/null 2>&1
}

system_rw() {
    mount -o remount,rw / >/dev/null 2>&1
}

card_rw() {
    # Zero Ichi (C) keeps the card's FAT drives read-only whenever it isn't saving
    # (so pulling the plug can't harm them) and makes them writable again when
    # it ends - this makes sure of it (e.g. after a crash), for what follows
    for d in /media/photos /boot/firmware; do
        if grep -q "^[^ ]* $d vfat ro[, ]" /proc/mounts 2>/dev/null; then
            mount -o remount,rw "$d" >/dev/null 2>&1
        fi
    done
}

stop_all() {
    # Stop everything we started and close the card's FAT partitions safely
    n=0                                    # (start-up jobs still running: let them end)
    for p in $JOBS; do
        while kill -0 "$p" 2>/dev/null && [ "$n" -lt 150 ]; do
            sleep 0.1
            n=$((n + 1))
        done
    done
    udevadm control --exit >/dev/null 2>&1
    swapoff -a >/dev/null 2>&1
    sync
    for d in /media/photos /boot/firmware; do
        if grep -q " $d " /proc/mounts 2>/dev/null; then
            umount "$d" >/dev/null 2>&1 || mount -o remount,ro "$d" >/dev/null 2>&1
        fi
    done
    sync
}

restart_now() {
    sync
    reboot -f -f >/dev/null 2>&1
    echo b > /proc/sysrq-trigger
    while :; do sleep 60; done            # (never end: init must not exit)
}

finish() {
    # $1 = poweroff | reboot: stop everything, close the card safely, switch off
    mark "$1"
    message                                # (Zero Ichi's last words stay on screen)
    stop_all
    late_fat_check
    mount -o remount,ro / >/dev/null 2>&1
    sync
    if [ "$1" = reboot ]; then
        restart_now
    fi
    poweroff -f -f >/dev/null 2>&1
    echo o > /proc/sysrq-trigger
    while :; do sleep 60; done
}

check_fat() {
    # A FAT partition ($1) that wasn't closed properly (power pulled without
    # KEY3) gets checked for errors - not now (on a big ZEROICHI drive that
    # takes seconds, and Zero Ichi would wait for it), but at the next power-off
    # with KEY3, once it is closed (late_fat_check). Linux reads it fine
    # meanwhile. (v52: zeroichi-lcd reads the flag - one small program instead of
    # dd, od and tr; an older one: those)
    st=""
    [ -x "$LCD" ] && st=$("$LCD" --fat-dirty "$1" 2>/dev/null)
    case "$st" in
        clean) return 0 ;;
        dirty)
            echo "$1" >> /run/zeroichi/fat-check
            mark "$1 wasn't closed properly - checked at power-off"
            return 0
            ;;
    esac
    if [ "$(dd if="$1" bs=1 skip=82 count=5 2>/dev/null)" = "FAT32" ]; then
        at=65
    else
        at=37
    fi
    state=$(od -An -tu1 -j"$at" -N1 "$1" 2>/dev/null | tr -d ' ')
    if [ -n "$state" ] && [ $((state & 1)) -eq 1 ]; then
        echo "$1" >> /run/zeroichi/fat-check
        mark "$1 wasn't closed properly - checked at power-off"
    fi
}

late_fat_check() {
    # (see check_fat) - the drives are closed now
    [ -f /run/zeroichi/fat-check ] || return 0
    while read -r dev; do
        if ! grep -q "^$dev " /proc/mounts 2>/dev/null; then
            fsck.fat -a -w "$dev" >/dev/null 2>&1
            mark "$dev checked"
        fi
    done < /run/zeroichi/fat-check
    rm -f /run/zeroichi/fat-check
    sync
}

format_photos() {
    # Right after "make room for photos": format the new partition once and
    # add it to /etc/fstab, so it is mounted at /media/photos from now on.
    f=$STATE/space-format                  # "device PARTUUID" (see make_space)
    [ -f "$f" ] || return 0
    read -r dev uuid < "$f"
    case "$dev" in
        /dev/mmcblk[0-9]p[3-9]) ;;
        *) rm -f "$f"; return 0 ;;
    esac
    n=0
    while [ ! -b "$dev" ] && [ "$n" -lt 30 ]; do sleep 0.1; n=$((n + 1)); done
    if [ -b "$dev" ]; then
        if [ "$(blkid -s TYPE -o value "$dev" 2>/dev/null)" != vfat ]; then
            mark "formatting the PHOTOS partition"
            if ! mkfs.fat -F 32 -n PHOTOS "$dev" >> "$STATE/space-log.txt" 2>&1; then
                echo "formatting $dev failed" >> "$STATE/space-log.txt"
                rm -f "$f"
                return 0
            fi
        fi
        mkdir -p /media/photos
        if ! grep -q "[[:space:]]/media/photos[[:space:]]" /etc/fstab; then
            echo "PARTUUID=$uuid  /media/photos  vfat  defaults,noatime,nofail,x-systemd.device-timeout=10s  0  2" >> /etc/fstab
        fi
        echo "PHOTOS partition ready ($dev)" >> "$STATE/space-log.txt"
    else
        echo "$dev did not appear - not formatted" >> "$STATE/space-log.txt"
    fi
    rm -f "$f"
    sync
}

relabel() {
    # (v53, once) the card's drive TOYCAM is called ZEROICHI now - renamed while
    # it isn't open yet ($1: the partition). Other names (bootfs, PHOTOS) stay.
    case "$(fatlabel "$1" 2>/dev/null)" in
        TOYCAM|"TOYCAM "*) fatlabel "$1" ZEROICHI >/dev/null 2>&1 && mark "drive TOYCAM renamed ZEROICHI" ;;
    esac
}

card_partitions() {
    # The card's FAT partitions: "bootfs" (settings, older photos) and PHOTOS
    format_photos
    # (v52: fstab read by the shell itself, the partition found by zeroichi-lcd -
    #  findfs looks through every disk; an older zeroichi-lcd: findfs)
    while read -r spec dir type _; do
        case "$spec" in ''|'#'*) continue ;; esac
        [ "$type" = vfat ] || continue
        dev=""
        [ -x "$LCD" ] && dev=$("$LCD" --partuuid "$spec" 2>/dev/null)
        [ -n "$dev" ] || dev=$(findfs "$spec" 2>/dev/null)
        if [ -z "$dev" ] && [ "$dir" = /boot/firmware ]; then
            dev=/dev/mmcblk0p1
        fi
        if [ -z "$dev" ] || [ ! -b "$dev" ]; then
            continue
        fi
        check_fat "$dev"
        [ -n "${RELABEL:-}" ] && relabel "$dev"
        [ -d "$dir" ] || mkdir -p "$dir" 2>/dev/null
        mount "$dir" >/dev/null 2>&1 || mount -t vfat "$dev" "$dir" >/dev/null 2>&1
        if [ "$dir" = /boot/firmware ] && [ -d "$dir/toycam" ] && [ ! -e "$SD" ]; then
            mv "$dir/toycam" "$SD" >/dev/null 2>&1 && mark "the card's toycam folder is called zeroichi now"
        fi
        mark "$dir ready"
    done < /etc/fstab
}

ram_tools() {
    # copy these programs (and the libraries they use) into the RAM root $R,
    # then check each one runs from there
    for b in "$@"; do
        p=$(command -v "$b") || "${FAIL:-space_fail}" "$b is missing"
        cp -L "$p" "$R/bin/$b" || "${FAIL:-space_fail}" "copying $b to RAM failed"
        for lib in $(ldd "$p" 2>/dev/null | grep -o '/[^ )]*'); do
            case "$lib" in
                *'$'*) continue ;;             # a name pattern, e.g. Pi OS's
            esac                               # "libarmmem-${PLATFORM}.so" (its real file is listed too)
            [ -e "$lib" ] || continue          # (a library really missing: the test below finds it)
            mkdir -p "$R${lib%/*}"
            cp -L "$lib" "$R$lib" || "${FAIL:-space_fail}" "copying $lib to RAM failed"
        done
    done
    for b in "$@"; do
        chroot "$R" "/bin/$b" --version >/dev/null 2>&1
        rc=$?                              # (127/126 = it can't run from RAM: stop here)
        if [ "$rc" -eq 127 ] || [ "$rc" -eq 126 ]; then
            "${FAIL:-space_fail}" "$b can't run from RAM"
        fi
    done
}

make_space() {
    # One-time "use the whole card for photos" (Zero Ichi works out the plan: its
    # space_plan). A partition can only be changed while nothing uses it, so
    # this runs from a copy of the few tools it needs in RAM (stage2_script).
    # MODE=move (the usual): the system partition is made smaller and copied to
    #   the end of the card. The space it leaves becomes one big FAT drive,
    #   ZEROICHI, which takes over from bootfs with all of bootfs's files (the
    #   Pi's start-up files, Zero Ichi, settings, photos): the card shows ONE
    #   drive on a computer, for everything.
    # MODE=fat: the same when the system is at the end of the card already
    #   (a first try that stopped half-way).
    # MODE=photos (cards too small to copy the system): an extra PHOTOS drive
    #   after the system partition, formatted at the next start (format_photos).
    # At every moment the card still starts: the partition table (one sector,
    # written at once) only changes after a copy has been checked.
    plan=$STATE/space-plan
    MODE="" DISK="" PART="" DISKID="" P1_START="" P1_SIZE="" ROOT_START="" ROOT_SIZE=""
    ROOT_KB="" ROOT_SECTORS="" ROOT_NEW_START="" FAT_START="" FAT_SECTORS="" P3_START=""
    while IFS='=' read -r k v; do
        case "$k" in
            MODE) MODE=$v ;;
            DISK) DISK=$v ;;
            PART) PART=$v ;;
            DISKID) DISKID=$v ;;
            P1_START) P1_START=$v ;;
            P1_SIZE) P1_SIZE=$v ;;
            ROOT_START) ROOT_START=$v ;;
            ROOT_SIZE) ROOT_SIZE=$v ;;
            ROOT_KB) ROOT_KB=$v ;;
            ROOT_SECTORS) ROOT_SECTORS=$v ;;
            ROOT_NEW_START) ROOT_NEW_START=$v ;;
            FAT_START) FAT_START=$v ;;
            FAT_SECTORS) FAT_SECTORS=$v ;;
            P3_START) P3_START=$v ;;
        esac
    done < "$plan"
    rm -f "$plan"
    # (the plan is checked against the card itself: any doubt - nothing is done)
    case "$MODE" in
        move) need="ROOT_NEW_START FAT_START FAT_SECTORS" ;;
        fat) need="FAT_START FAT_SECTORS" ;;
        photos) need="P3_START" ;;
        *) return 1 ;;
    esac
    case "$DISK" in /dev/mmcblk[0-9]) ;; *) return 1 ;; esac
    [ "$PART" = "${DISK}p2" ] || return 1
    case "$DISKID" in
        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
        *) return 1 ;;
    esac
    for k in P1_START P1_SIZE ROOT_START ROOT_SIZE ROOT_KB ROOT_SECTORS $need; do
        eval "n=\${$k}"
        case "$n" in ''|*[!0-9]*) return 1 ;; esac
    done
    [ "$MODE" = fat ] && ROOT_NEW_START=$ROOT_START
    d=${DISK#/dev/}
    disk_size=$(cat "/sys/block/$d/size" 2>/dev/null)
    case "$disk_size" in ''|*[!0-9]*) return 1 ;; esac
    for v in "p1/start $P1_START" "p1/size $P1_SIZE" "p2/start $ROOT_START" "p2/size $ROOT_SIZE"; do
        [ "$(cat "/sys/block/$d/$d${v%% *}" 2>/dev/null)" = "${v#* }" ] || return 1
    done
    [ "$ROOT_SECTORS" -eq $((ROOT_KB * 2)) ] || return 1
    p1_end=$((P1_START + P1_SIZE))
    case "$MODE" in
        move)
            [ $((ROOT_KB % 4096)) -eq 0 ] && [ $((ROOT_NEW_START % 8192)) -eq 0 ] || return 1
            [ "$ROOT_SECTORS" -le "$ROOT_SIZE" ] || return 1
            [ "$ROOT_NEW_START" -ge $((ROOT_START + ROOT_SECTORS)) ] || return 1
            [ $((ROOT_NEW_START + ROOT_SECTORS)) -le "$disk_size" ] || return 1
            [ "$FAT_START" -ge "$p1_end" ] || return 1
            [ $((FAT_START + FAT_SECTORS)) -le "$ROOT_NEW_START" ] || return 1 ;;
        fat)
            [ "$ROOT_SECTORS" -eq "$ROOT_SIZE" ] || return 1
            [ "$FAT_START" -ge "$p1_end" ] || return 1
            [ $((FAT_START + FAT_SECTORS)) -le "$ROOT_START" ] || return 1 ;;
        photos)
            [ "$ROOT_SECTORS" -le "$ROOT_SIZE" ] || return 1
            [ "$P3_START" -ge $((ROOT_START + ROOT_SECTORS)) ] || return 1
            [ "$P3_START" -lt "$disk_size" ] || return 1 ;;
    esac
    mark "making room for photos ($MODE)"
    message                                # "making room for photos..." stays on screen
    echo heartbeat 2>/dev/null > /sys/class/leds/ACT/trigger   # green light: working
    stop_all
    R=/run/space
    mkdir -p "$R"
    mount -t tmpfs -o mode=0755 tmpfs "$R" || space_fail "no room in RAM"
    mkdir -p "$R/oldroot" "$R/proc" "$R/sys" "$R/dev" "$R/mnt" "$R/bin" "$R/steps"
    ram_tools sh e2fsck resize2fs sfdisk partx mount umount sync sleep cat rm mkdir dd cmp cp find mkfs.fat
    # the screens for each step (made by Zero Ichi) and the program that shows them
    if [ -x "$LCD" ] && [ -f "$STATE/lcd.conf" ]; then
        cp "$LCD" "$R/bin/zeroichi-lcd" && cp "$STATE/lcd.conf" "$R/lcd.conf"
        cp "$STATE"/lcd-*.conf "$R/" 2>/dev/null       # (this Pi's own screen - see zeroichi-lcd)
        cp "$STATE"/steps/*.rgb565 "$R/steps/" 2>/dev/null
    fi
    rm -rf "$STATE/steps"
    {
        for k in MODE DISK DISKID P1_START P1_SIZE ROOT_START ROOT_SIZE ROOT_KB ROOT_SECTORS \
                 ROOT_NEW_START FAT_START FAT_SECTORS P3_START; do
            eval "echo $k=\${$k}"
        done
    } > "$R/plan" || space_fail "no room in RAM"
    stage2_script > "$R/stage2" || space_fail "no room in RAM"
    mount --make-rprivate / >/dev/null 2>&1
    cd "$R" || space_fail "no room in RAM"
    if pivot_root . oldroot; then
        exec /bin/sh /stage2               # (from RAM: the system partition is free)
    fi
    cd /
    space_fail "switching to RAM (pivot_root) failed"
}

space_failed() {
    # one more failed try (Zero Ichi stops offering it at start-up after two)
    f=$(cat "$STATE/space-fails" 2>/dev/null)
    case "$f" in ''|*[!0-9]*) f=0 ;; esac
    echo $((f + 1)) > "$STATE/space-fails"
}

space_fail() {
    echo "$1 - nothing was changed" >> "$STATE/space-log.txt"
    space_failed
    restart_now
}

stage2_script() {
    # The part of make_space that runs from RAM (after pivot_root; tests run it
    # on a disk image with TEST=1). Reads its numbers from /plan.
    cat <<'STAGE2'
#!/bin/sh
# Zero Ichi: making room for photos - running from RAM, the card is not in use
PATH=${TEST_PATH:-/bin}
export PATH
. "${TEST_ROOT:-}/plan"
X=${TEST_ROOT:-}                           # (tests: where this "RAM" is)
L=$X/log.txt
d=${DISK#/dev/}
FORMAT=""
say() { echo "$*" >> "$L"; }
step() {
    if [ -f "$X/steps/$1.rgb565" ] && [ -x "$X/bin/zeroichi-lcd" ]; then
        "$X/bin/zeroichi-lcd" "$X/lcd.conf" "$X/steps/$1.rgb565" >/dev/null 2>&1
    fi
    say "- $1"
}
restart() {
    sync
    if [ -n "$TEST" ]; then exit 0; fi
    echo b > /proc/sysrq-trigger
    while :; do sleep 60; done
}
finish_() {
    # $1 = ok | fail: the report goes onto the system partition, then restart
    n=0
    while ! mount -t ext4 "${DISK}p2" "$X/mnt" 2>/dev/null; do
        n=$((n + 1))
        if [ $n -ge 40 ]; then
            say "the system partition could not be opened for the report"
            restart
        fi
        sleep 0.25
    done
    S=$X/mnt/var/lib/zeroichi
    mkdir -p "$S"
    if [ "$1" = ok ]; then
        rm -f "$S/space-fails"
        if [ -n "$FORMAT" ]; then echo "$FORMAT" > "$S/space-format"; fi
    else
        f=$(cat "$S/space-fails" 2>/dev/null)
        case "$f" in ''|*[!0-9]*) f=0 ;; esac
        echo $((f + 1)) > "$S/space-fails"
    fi
    cat "$L" >> "$S/space-log.txt"
    umount "$X/mnt"
    restart
}
unchanged() { say "$1 - nothing was changed"; finish_ fail; }

parts_are() {
    # does the kernel see exactly these partitions ("start=S, size=N, ..." each)?
    i=0
    for e in "$@"; do
        i=$((i + 1))
        s=${e#start=}; s=${s%%,*}
        z=${e#*size=}; z=${z%%,*}
        [ -b "${DISK}p$i" ] || return 1
        [ "$(cat "/sys/block/$d/${d}p$i/start" 2>/dev/null)" = "$s" ] || return 1
        [ "$(cat "/sys/block/$d/${d}p$i/size" 2>/dev/null)" = "$z" ] || return 1
    done
    i=$((i + 1))
    [ ! -e "/sys/block/$d/${d}p$i" ]
}
table() {
    # write the whole partition table at once (the arguments: one partition each)
    {
        echo "label: dos"
        echo "label-id: 0x$DISKID"
        echo "unit: sectors"
        echo
        for e in "$@"; do echo "$e"; done
    } > "$X/table.txt"
    cat "$X/table.txt" >> "$L"
    sfdisk --wipe never --wipe-partitions never -q "$DISK" < "$X/table.txt" >> "$L" 2>&1 || return 1
    sync
    n=0
    while ! parts_are "$@"; do            # (until the kernel has the new partitions)
        n=$((n + 1))
        if [ $n -eq 4 ]; then partx -d "$DISK" >> "$L" 2>&1; partx -a "$DISK" >> "$L" 2>&1; fi
        if [ $n -ge 40 ]; then say "the kernel didn't take the new partitions"; return 1; fi
        sleep 0.25
    done
}

if [ -z "$TEST" ]; then
    mount --move /oldroot/proc /proc
    mount --move /oldroot/sys /sys
    mount --move /oldroot/dev /dev
    umount /oldroot/run 2>/dev/null
    umount /oldroot/tmp 2>/dev/null
    if ! umount /oldroot >> "$L" 2>&1; then
        say "the system partition is still in use - nothing was changed"
        cat "$L" >> /oldroot/var/lib/zeroichi/space-log.txt
        mount -o remount,ro /oldroot
        restart
    fi
fi
T1=$(sfdisk --part-type "$DISK" 1 2>/dev/null)
T2=$(sfdisk --part-type "$DISK" 2 2>/dev/null)
case "$T1" in [0-9a-fA-F]|[0-9a-fA-F][0-9a-fA-F]) ;; *) T1=c ;; esac
case "$T2" in [0-9a-fA-F]|[0-9a-fA-F][0-9a-fA-F]) ;; *) T2=83 ;; esac
P1="start=$P1_START, size=$P1_SIZE, type=$T1"
say "make room for photos ($MODE): $(cat "/sys/block/$d/size") sectors, $P1, system $ROOT_START+$ROOT_SIZE"

if [ "$MODE" = move ] || [ "$MODE" = photos ]; then
    step check
    e2fsck -f -y "${DISK}p2" >> "$L" 2>&1
    rc=$?
    if [ $rc -ge 4 ]; then unchanged "the check found problems it could not fix ($rc)"; fi
    step shrink
    resize2fs "${DISK}p2" "${ROOT_KB}K" >> "$L" 2>&1 || unchanged "the system could not be made smaller"
fi

if [ "$MODE" = photos ]; then
    step parts
    size=$(cat "/sys/block/$d/size")
    table "$P1" "start=$ROOT_START, size=$ROOT_SECTORS, type=$T2" \
        "start=$P3_START, size=$((size - P3_START)), type=c" ||
        unchanged "the partitions could not be changed"
    FORMAT="${DISK}p3 $DISKID-03"
    say "done: PHOTOS partition added"
    finish_ ok
fi

if [ "$MODE" = move ]; then
    # the system's copy at the end of the card (the original stays as it is
    # until the copy has been checked)
    step move
    dd if="${DISK}p2" of="$DISK" bs=4M count=$((ROOT_KB / 4096)) seek=$((ROOT_NEW_START / 8192)) \
        conv=fsync >> "$L" 2>&1 || unchanged "copying the system failed"
    sync
    echo 3 2>/dev/null > /proc/sys/vm/drop_caches
    a=$((ROOT_NEW_START * 512))
    e=$((ROOT_KB * 1024 - 67108864))
    if ! cmp -n 67108864 "${DISK}p2" "$DISK" 0 "$a" >> "$L" 2>&1 ||
            ! cmp -n 67108864 "${DISK}p2" "$DISK" "$e" $((a + e)) >> "$L" 2>&1; then
        unchanged "the system's copy doesn't match"
    fi
    OLD="start=$ROOT_START, size=$ROOT_SIZE, type=$T2"
    NEW="start=$ROOT_NEW_START, size=$ROOT_SECTORS, type=$T2"
    if ! table "$P1" "$NEW"; then
        table "$P1" "$OLD"
        unchanged "the system's new place could not be set"
    fi
    e2fsck -f -y "${DISK}p2" >> "$L" 2>&1
    rc=$?
    if [ $rc -ge 4 ]; then
        table "$P1" "$OLD"
        unchanged "the system's copy has problems ($rc)"
    fi
    say "the system is at the end of the card now"
    ROOT_START=$ROOT_NEW_START
    ROOT_SIZE=$ROOT_SECTORS
fi

# the new drive in the free space, with all of bootfs's files
SYS="start=$ROOT_START, size=$ROOT_SIZE, type=$T2"
FAT="start=$FAT_START, size=$FAT_SECTORS, type=c"
back() {
    say "$1 - bootfs stays as it was"
    umount "$X/new" 2>/dev/null
    umount "$X/old" 2>/dev/null
    table "$P1" "$SYS"
    finish_ fail
}
step drive
table "$P1" "$SYS" "$FAT" || back "the new drive could not be added"
# (the space held the system before: clear its old start, so nothing mistakes
# the new drive for the old system partition)
dd if=/dev/zero of="${DISK}p3" bs=1M count=4 conv=fsync >> "$L" 2>&1 || back "the new drive could not be written"
mkfs.fat -F 32 -n ZEROICHI -h "$FAT_START" "${DISK}p3" >> "$L" 2>&1 || back "formatting the new drive failed"
mkdir -p "$X/old" "$X/new"
mount -t vfat -o ro "${DISK}p1" "$X/old" >> "$L" 2>&1 || back "bootfs could not be opened"
mount -t vfat "${DISK}p3" "$X/new" >> "$L" 2>&1 || back "the new drive could not be opened"
for f in "$X/old"/* "$X/old"/.[!.]*; do    # (each item: the drive's own top folder keeps its times)
    if [ -e "$f" ]; then
        cp -r --preserve=timestamps "$f" "$X/new/" >> "$L" 2>&1 || back "copying bootfs's files failed"
    fi
done
umount "$X/new" || back "the new drive could not be closed"
sync
echo 3 2>/dev/null > /proc/sys/vm/drop_caches
mount -t vfat -o ro "${DISK}p3" "$X/new" >> "$L" 2>&1 || back "the new drive could not be opened again"
(cd "$X/old" && find . -type f) > "$X/files.txt" || back "bootfs could not be read"
bad=0
n=0
while IFS= read -r f; do
    n=$((n + 1))
    if ! cmp -s "$X/old/$f" "$X/new/$f"; then
        bad=$((bad + 1))
        say "different: $f"
    fi
done < "$X/files.txt"
umount "$X/new"
umount "$X/old"
if [ $bad -gt 0 ] || [ $n -eq 0 ]; then back "$bad of $n files are different"; fi
say "$n files copied and checked"
# from now on the new drive is the one the Pi starts from (and the only one)
table "$FAT" "$SYS" || back "the new drive could not be made the start-up drive"
say "done: one drive for everything"
finish_ ok
STAGE2
}

image_fail() {
    echo "$1" >> "$STATE/image-log.txt"
    restart_now
}

make_image() {
    # "Make a card image" (asked for by Zero Ichi: MAKE-IMAGE.txt on the card).
    # From RAM (like make_space), so the system partition can be read as a
    # whole while nothing uses it: what a new card doesn't need is left out
    # (download caches, logs, the start files of Pi models a Zero Ichi isn't -
    # see slim), its file system is made as small as is sensible and written,
    # with the start-up drive ZEROICHI, into zeroichi-card.img.xz (packed small,
    # for downloading) on the ZEROICHI drive, then grown back. Flashing that file
    # onto a new card gives a ready Zero Ichi. This card's photos, settings and
    # presets are not in it, and stay here.
    plan=$STATE/image-plan
    DISK="" DISKID="" ROOT_START="" ROOT_SIZE="" ROOT_KB="" OUT_DEV="" BOOT_DEV="" LAYOUT="" COMP="" TUNE=""
    while IFS='=' read -r k v; do
        case "$k" in
            DISK) DISK=$v ;;
            DISKID) DISKID=$v ;;
            ROOT_START) ROOT_START=$v ;;
            ROOT_SIZE) ROOT_SIZE=$v ;;
            ROOT_KB) ROOT_KB=$v ;;
            OUT_DEV) OUT_DEV=$v ;;
            BOOT_DEV) BOOT_DEV=$v ;;
            LAYOUT) LAYOUT=$v ;;
        esac
    done < "$plan"
    rm -f "$plan"
    case "$LAYOUT" in end|start) ;; *) return 1 ;; esac
    case "$DISK" in /dev/mmcblk[0-9]) ;; *) return 1 ;; esac
    case "$OUT_DEV" in "$DISK"p[13-9]) ;; *) return 1 ;; esac
    case "$BOOT_DEV" in "$DISK"p[13-9]) ;; *) return 1 ;; esac
    case "$DISKID" in
        [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
        *) return 1 ;;
    esac
    for n in "$ROOT_START" "$ROOT_SIZE" "$ROOT_KB"; do
        case "$n" in ''|*[!0-9]*) return 1 ;; esac
    done
    d=${DISK#/dev/}
    [ "$(cat "/sys/block/$d/${d}p2/start" 2>/dev/null)" = "$ROOT_START" ] || return 1
    [ "$(cat "/sys/block/$d/${d}p2/size" 2>/dev/null)" = "$ROOT_SIZE" ] || return 1
    [ $((ROOT_KB % 4096)) -eq 0 ] && [ $((ROOT_KB * 2)) -le "$ROOT_SIZE" ] || return 1
    mark "making a card image"
    message
    echo heartbeat 2>/dev/null > /sys/class/leds/ACT/trigger
    stop_all
    FAIL=image_fail
    COMP=gzip                              # xz packs it about a third smaller (gzip: if missing)
    command -v xz >/dev/null 2>&1 && COMP=xz
    command -v tune2fs >/dev/null 2>&1 && TUNE=tune2fs
    R=/run/space
    mkdir -p "$R"
    mount -t tmpfs -o mode=0755 tmpfs "$R" || image_fail "no room in RAM"
    mkdir -p "$R/oldroot" "$R/proc" "$R/sys" "$R/dev" "$R/mnt" "$R/bin" "$R/out" "$R/bootfs" "$R/fat" "$R/steps"
    # shellcheck disable=SC2086
    ram_tools sh e2fsck resize2fs e2image $COMP $TUNE sfdisk mount umount sync sleep cat rm mkdir dd cp \
        mkfs.fat truncate mv od tr sed du
    # the screens for each step and the end (made by Zero Ichi) and the program that shows them
    if [ -x "$LCD" ] && [ -f "$STATE/lcd.conf" ]; then
        cp "$LCD" "$R/bin/zeroichi-lcd" && cp "$STATE/lcd.conf" "$R/lcd.conf"
        cp "$STATE"/lcd-*.conf "$R/" 2>/dev/null       # (this Pi's own screen - see zeroichi-lcd)
        cp "$STATE"/steps/img-*.rgb565 "$R/steps/" 2>/dev/null
    fi
    rm -rf "$STATE/steps"
    {
        for k in DISK DISKID ROOT_START ROOT_SIZE ROOT_KB OUT_DEV BOOT_DEV LAYOUT COMP; do
            eval "echo $k=\${$k}"
        done
    } > "$R/plan" || image_fail "no room in RAM"
    image_script > "$R/stage2" || image_fail "no room in RAM"
    mount --make-rprivate / >/dev/null 2>&1
    cd "$R" || image_fail "no room in RAM"
    if pivot_root . oldroot; then
        exec /bin/sh /stage2
    fi
    cd /
    image_fail "switching to RAM (pivot_root) failed"
}

image_script() {
    # The part of make_image that runs from RAM (tests: TEST=1, see stage2_script)
    cat <<'IMAGE'
#!/bin/sh
# Zero Ichi: making a card image - running from RAM, the card is not in use
PATH=${TEST_PATH:-/bin}
export PATH
. "${TEST_ROOT:-}/plan"
X=${TEST_ROOT:-}
L=$X/log.txt
case "$COMP" in
    xz) NAME=zeroichi-card.img.xz; PACK="xz -1 -T1" ;;   # (-1: about 30-40 minutes on a Pi Zero)
    *) NAME=zeroichi-card.img.gz; PACK="gzip -6" ;;
esac
read -r T0 _ < /proc/uptime
say() { echo "$*" >> "$L"; }
show() {           # a screen Zero Ichi made ($1), with the progress bar at $2 % (if given)
    if [ -f "$X/steps/$1.rgb565" ] && [ -x "$X/bin/zeroichi-lcd" ]; then
        if [ -n "$2" ]; then
            "$X/bin/zeroichi-lcd" --update --progress "$2" "$X/lcd.conf" "$X/steps/$1.rgb565" >/dev/null 2>&1
        else
            "$X/bin/zeroichi-lcd" --update "$X/lcd.conf" "$X/steps/$1.rgb565" >/dev/null 2>&1
        fi
    fi
}
STEP=img-check
PCT=-1
progress() {       # how far it is, in % of the whole job (redrawn when it changes)
    [ "$1" -gt "$PCT" ] || return 0
    PCT=$1
    show "$STEP" "$1"
}
step() {           # the next step's screen ($1), at $2 %
    STEP=$1
    PCT=-1
    progress "$2"
    say "- $1"
}
rchar() {          # how much program $1 has read so far (bytes)
    { while read -r k v; do [ "$k" = "rchar:" ] && echo "$v"; done < "/proc/$1/io"; } 2>/dev/null
}
u16() { od -An -tu2 -j"$2" -N2 "$1" | tr -d ' '; }
p32() {
    v=$3
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $((v & 255)) $((v >> 8 & 255)) $((v >> 16 & 255)) $((v >> 24 & 255)))" |
        dd of="$1" bs=1 seek="$2" count=4 conv=notrunc 2>/dev/null
}
fat_set_size() {   # FAT32 $1: now $2 sectors (its FAT tables hold more), free count: work it out
    bk=$(u16 "$1" 50)
    fsi=$(u16 "$1" 48)
    p32 "$1" 32 "$2"
    if [ "$bk" -gt 0 ]; then p32 "$1" $((bk * 512 + 32)) "$2"; fi
    p32 "$1" $((fsi * 512 + 488)) 4294967295
    if [ "$bk" -gt 0 ]; then p32 "$1" $(((bk + fsi) * 512 + 488)) 4294967295; fi
}
flavor() {         # a kernel's kind, from its version: v6 (Pi Zero, Zero W, 1), v7 (Zero 2 W, 2, 3),
    case "$1" in   # v7l / v8 / 2712 (Pi 4, 400, 5)
        *-2712*) echo 2712 ;;
        *-v8*) echo v8 ;;
        *-v7l*) echo v7l ;;
        *-v7*) echo v7 ;;
        *) echo v6 ;;
    esac
}
RUN=$(flavor "${TEST_OSRELEASE:-$(cat /proc/sys/kernel/osrelease)}")
KEEP=" v6 v7 "                     # (Zero Ichi is a Pi Zero camera; this Pi's own kind is kept too)
case "$KEEP" in *" $RUN "*) ;; *) KEEP="$KEEP$RUN " ;; esac
SKIP=""                            # start files on ZEROICHI of the other kinds
for f in v7l:kernel7l.img v7l:initramfs7l v8:kernel8.img v8:initramfs8 2712:kernel_2712.img \
         2712:initramfs_2712; do
    case "$KEEP" in *" ${f%%:*} "*) ;; *) SKIP="$SKIP ${f#*:}" ;; esac
done
SKIP="$SKIP "
slim() {
    # The system partition ($1, mounted): leave out what a new card doesn't
    # need - this card's own settings and logs, download caches and lists
    # (they come back by themselves: "apt update"), the swap file (made again
    # at a normal start-up) and the kernels of Pi models a Zero Ichi isn't.
    M=$1
    S=$M/var/lib/zeroichi
    mkdir -p "$X/keep"                     # (this card's own screen settings: back after - finish_)
    cp "$S"/lcd*.conf "$X/keep/" 2>/dev/null
    rm -f "$S/app/state.json" "$S/clock" "$S/space-fails" "$S/space-log.txt" "$S/image-log.txt" \
        "$S/message.rgb565" "$S/normal-start" "$S"/lcd-*.conf
    rm -rf "$S/steps" "$M/var/lib/zeroichi-offline"
    if [ -f "$S/lcd.conf" ]; then          # (a new card starts with the 240x240 screen - settings.txt)
        printf 'controller st7789\nwidth 240\nheight 240\nmadctl 112\ninvert 1\ndc 25\nrst 27\nbl 24\ncs 0\nspeed 40000000\nflip 0\n' \
            > "$S/lcd.conf"
    fi
    rm -rf "$M"/var/cache/apt/archives/*.deb "$M"/var/cache/apt/archives/partial/* "$M"/var/cache/apt/*.bin
    for f in "$M"/var/lib/apt/lists/*; do
        if [ -f "$f" ] && [ "${f##*/}" != lock ]; then rm -f "$f"; fi
    done
    rm -f "$M/var/swap"
    rm -rf "$M"/var/log/journal/* "$M"/var/tmp/* "$M"/tmp/* "$M/root/.cache" "$M"/home/*/.cache
    for f in "$M"/var/log/*.gz "$M"/var/log/*.[0-9] "$M"/var/log/*/*.gz "$M"/var/log/*/*.[0-9]; do
        rm -f "$f"
    done
    # (0.1.0) the smallest image: what a camera never reads - manuals, docs
    # (not the licences) and translations, the firmware of Wi-Fi / Bluetooth chips (Zero Ichi
    # never uses them - see bootopt's radios_off) and the Pi 4 / 5 boot
    # EEPROM images - and a note for the package manager not to bring the
    # manuals and docs back with a later install
    # (each package's copyright file stays: its licence, which must go with it.
    #  Only the shell and rm here: the image is made from RAM, with few tools)
    for d in "$M"/usr/share/doc/*; do
        [ -d "$d" ] || { rm -f "$d"; continue; }
        for f in "$d"/* "$d"/.[!.]*; do
            [ -e "$f" ] || [ -L "$f" ] || continue
            [ "${f##*/}" = copyright ] || rm -rf "$f"
        done
    done
    rm -rf "$M"/usr/share/man/* "$M"/usr/share/info/* "$M"/usr/share/lintian \
        "$M"/usr/share/linda "$M"/var/cache/man/* "$M"/var/cache/debconf/*-old "$M"/var/lib/dpkg/*-old
    for d in "$M"/usr/share/locale/*; do
        case "${d##*/}" in en|en_*|locale.alias) ;; *) rm -rf "$d" ;; esac
    done
    # firmware for other chips (Wi-Fi, Bluetooth, USB gadgets, graphics cards...):
    # the Pi Zero's camera and screen need none (the Pi's own is on the start-up
    # drive) - all of it but the radio rules file goes, and the Pi 4 / 5 EEPROMs
    for fw in "$M"/usr/lib/firmware "$M"/lib/firmware; do
        [ -d "$fw" ] && [ ! -L "$fw" ] || continue
        for f in "$fw"/* "$fw"/.[!.]*; do
            [ -e "$f" ] || [ -L "$f" ] || continue
            case "${f##*/}" in regulatory.db*|raspberrypi) ;; *) rm -rf "$f" ;; esac
        done
        rm -rf "$fw"/raspberrypi/bootloader*
    done
    if [ -d "$M/etc/dpkg/dpkg.cfg.d" ]; then
        printf '%s\n' "# Zero Ichi: no manuals, docs or translations (a camera never reads them)" \
            "path-exclude /usr/share/doc/*" "path-exclude /usr/share/man/*" "path-exclude /usr/share/info/*" \
            "path-exclude /usr/share/locale/*" "path-include /usr/share/locale/en*" \
            "path-include /usr/share/locale/locale.alias" "path-include /usr/share/doc/*/copyright" \
            > "$M/etc/dpkg/dpkg.cfg.d/zeroichi-small"
    fi
    MOD=$M/usr/lib/modules
    [ -d "$MOD" ] || MOD=$M/lib/modules
    for d in "$MOD"/*; do
        [ -d "$d" ] || continue
        v=${d##*/}
        case "$KEEP" in
            *" $(flavor "$v") "*)
                # (a kept kernel: without the drivers a camera never loads - Wi-Fi,
                #  Bluetooth, sound, USB webcams / TV tuners / network adapters,
                #  firewalls. They only load for such hardware, which isn't there)
                k=$d/kernel
                rm -rf "$k"/drivers/net/wireless "$k"/drivers/bluetooth "$k"/net/bluetooth "$k"/net/wireless \
                    "$k"/net/mac80211 "$k"/sound "$k"/drivers/media/usb "$k"/drivers/media/dvb-frontends \
                    "$k"/drivers/media/dvb-core "$k"/drivers/media/rc "$k"/drivers/media/tuners \
                    "$k"/drivers/net/usb "$k"/drivers/net/ethernet "$k"/drivers/net/can "$k"/net/can \
                    "$k"/drivers/net/ieee802154 "$k"/net/ieee802154 "$k"/net/netfilter "$k"/net/ipv4/netfilter \
                    "$k"/net/ipv6/netfilter "$k"/net/bridge "$k"/drivers/isdn "$k"/drivers/staging/rtl* \
                    "$k"/drivers/staging/greybus "$k"/drivers/staging/vc04_services/bcm2835-audio
                continue ;;
        esac
        rm -rf "$d" "$M/usr/lib/linux-image-$v" "$M/var/lib/initramfs-tools/$v"
        rm -f "$M/boot/vmlinuz-$v" "$M/boot/initrd.img-$v" "$M/boot/config-$v" "$M/boot/System.map-$v"
        say "left out: kernel $v (Pi 4 / 400 / 5)"
    done
}
restart() {
    sync
    if [ -n "$TEST" ]; then exit 0; fi
    echo s > /proc/sysrq-trigger
    sleep 1
    echo b > /proc/sysrq-trigger
    while :; do sleep 60; done
}
power_off() {      # (the screen keeps its last picture until the power is gone)
    sync
    if [ -n "$TEST" ]; then exit 0; fi
    echo s > /proc/sysrq-trigger
    sleep 1
    echo o > /proc/sysrq-trigger
    while :; do sleep 60; done
}
finish_() {
    # the system partition back to its full size, the report onto it, then
    # switch off (so it's clear it's done) - or "didn't work" and restart
    # (Zero Ichi carries on as before). The image is complete before this: the
    # screen says so straight away ("Card image ready! - switching off"), as
    # the last part (growing the system back) can take a few minutes.
    case "$1" in "RESULT: ok"*) show img-done ;; esac
    echo "$1" >> "$L"
    cp "$L" "$X/out/zeroichi-card-log.txt" 2>/dev/null      # (on the card: easy to send)
    umount "$X/fat" 2>/dev/null
    umount "$X/bootfs" 2>/dev/null
    umount "$X/out" 2>/dev/null
    read -r g0 _ < /proc/uptime
    resize2fs "${DISK}p2" >> "$L" 2>&1 || say "growing the system partition back didn't work"
    read -r t _ < /proc/uptime
    say "growing the system back took $(( ${t%.*} - ${g0%.*} )) s"
    say "took $(( (${t%.*} - ${T0%.*}) / 60 )) minutes"
    echo "$1" >> "$L"
    if mount -t ext4 "${DISK}p2" "$X/mnt" 2>/dev/null; then
        mkdir -p "$X/mnt/var/lib/zeroichi"
        cat "$L" >> "$X/mnt/var/lib/zeroichi/image-log.txt"
        cp "$X"/keep/lcd*.conf "$X/mnt/var/lib/zeroichi/" 2>/dev/null
        umount "$X/mnt"
    fi
    case "$1" in
        "RESULT: ok"*) power_off ;;
    esac
    show img-fail
    restart
}

if [ -z "$TEST" ]; then
    mount --move /oldroot/proc /proc
    mount --move /oldroot/sys /sys
    mount --move /oldroot/dev /dev
    umount /oldroot/run 2>/dev/null
    umount /oldroot/tmp 2>/dev/null
    if ! umount /oldroot >> "$L" 2>&1; then
        say "the system partition is still in use"
        mount -o remount,rw /oldroot 2>/dev/null
        cat "$L" >> /oldroot/var/lib/zeroichi/image-log.txt
        mount -o remount,ro /oldroot
        restart
    fi
fi
say "card image: system $ROOT_KB KiB, from $BOOT_DEV, onto $OUT_DEV ($COMP; kept kernels:$KEEP)"
step img-check 0
e2fsck -f -y "${DISK}p2" >> "$L" 2>&1
[ $? -lt 4 ] || finish_ "RESULT: failed - the system partition has problems"
step img-slim 3
if mount -t ext4 "${DISK}p2" "$X/mnt" 2>> "$L"; then
    sizes() {      # (the image log: what takes the room - for making it smaller still)
        say "$1:"
        for d in usr/lib/modules usr/lib/firmware usr/lib/python3 usr/lib/arm-linux-gnueabihf usr/share \
                  usr/bin var opt home; do
            [ -d "$X/mnt/$d" ] && say "  $(du -sk "$X/mnt/$d" 2>/dev/null)"
        done
        say "  $(du -sk "$X/mnt" 2>/dev/null)"
    }
    sizes "system before"
    slim "$X/mnt"
    sizes "system after"
    umount "$X/mnt" || finish_ "RESULT: failed - the system partition is busy"
fi
progress 4
e2fsck -f -y "${DISK}p2" >> "$L" 2>&1
[ $? -lt 4 ] || finish_ "RESULT: failed - the system partition has problems"
progress 6
resize2fs "${DISK}p2" "${ROOT_KB}K" >> "$L" 2>&1 || finish_ "RESULT: failed - the system could not be made smaller"
progress 9
# a fresh journal: the old one is full of old changes (they would go into the
# image); a new one is all zeros, which packs to almost nothing
if command -v tune2fs >/dev/null 2>&1 && tune2fs -O ^has_journal "${DISK}p2" >> "$L" 2>&1; then
    tune2fs -j "${DISK}p2" >> "$L" 2>&1 || tune2fs -j "${DISK}p2" >> "$L" 2>&1 ||
        say "the system partition has no journal now (it works without)"
fi
progress 10
mount -t vfat "$OUT_DEV" "$X/out" >> "$L" 2>&1 || finish_ "RESULT: failed - the photo drive could not be opened"
B=$X/out
if [ "$BOOT_DEV" != "$OUT_DEV" ]; then
    mount -t vfat -o ro "$BOOT_DEV" "$X/bootfs" >> "$L" 2>&1 || finish_ "RESULT: failed - bootfs could not be opened"
    B=$X/bootfs
fi
rm -f "$X/out/zeroichi-card.img.gz" "$X/out/zeroichi-card.img.xz" "$X/out/.zeroichi-boot.fat" "$X/out/.$NAME"
# The start-up drive (ZEROICHI) with the start-up files and Zero Ichi - not the
# photos, logs, presets or this card's settings.
# LAYOUT=end (the usual): the system first, ZEROICHI after it - its FAT tables
#   are made for a 128 GB drive although it starts at 1.25 GB, so on a flashed
#   card it just grows to the card's end at the first start (grow_fat):
#   no making room for photos needed.
# LAYOUT=start (a system too big for that): ZEROICHI (512 MB) first, the
#   flashed card makes room for photos at its first start.
if [ "$LAYOUT" = end ]; then
    FAT_MB=1280
    FAT_START=$((8192 + ROOT_KB * 2))
    ROOT_AT=8192
else
    FAT_MB=512
    FAT_START=8192
    ROOT_AT=1056768
fi
F=$X/out/.zeroichi-boot.fat
truncate -s ${FAT_MB}M "$F" || finish_ "RESULT: failed - no room on the photo drive"
if [ "$LAYOUT" = end ]; then
    mkfs.fat -F 32 -s 32 -n ZEROICHI -h "$FAT_START" "$F" 134217728 >> "$L" 2>&1 ||
        finish_ "RESULT: failed - formatting"
    fat_set_size "$F" $((FAT_MB * 2048))
else
    mkfs.fat -F 32 -n ZEROICHI -h "$FAT_START" "$F" >> "$L" 2>&1 || finish_ "RESULT: failed - formatting"
fi
mount -t vfat -o loop "$F" "$X/fat" >> "$L" 2>&1 || finish_ "RESULT: failed - loop"
for f in "$B"/* "$B"/.[!.]*; do
    [ -e "$f" ] || continue
    n=${f##*/}
    case "$n" in
        photos|offline|"System Volume Information"|zeroichi-card.*|*.img.gz|*.img.xz|.zeroichi-boot.fat) continue ;;
        .Trash*|.Spotlight*|.fseventsd) continue ;;
    esac
    case "$SKIP" in *" $n "*) say "left out: $n (Pi 4 / 400 / 5)"; continue ;; esac
    cp -r --preserve=timestamps "$f" "$X/fat/" >> "$L" 2>&1 || finish_ "RESULT: failed - copying the start-up files"
done
for f in zeroichi-log.txt boot-time.txt state.json zeroichi-diagnostics.txt MAKE-IMAGE.txt make-image.txt; do
    rm -f "$X/fat/zeroichi/$f"
done
# a new card starts with the 240x240 screen (each Zero Ichi then remembers its own)
if [ -f "$X/fat/zeroichi/settings.txt" ]; then
    sed 's/^[[:space:]]*display[[:space:]]*=.*/display = 1.3inch-hat/' "$X/fat/zeroichi/settings.txt" \
        > "$X/settings.new" && cp "$X/settings.new" "$X/fat/zeroichi/settings.txt"
    rm -f "$X/settings.new"
fi
rm -rf "$X/fat/zeroichi/presets"
mkdir -p "$X/fat/photos"
progress 11
umount "$X/fat" || finish_ "RESULT: failed - closing the start-up drive"
# the partition table: ZEROICHI is always the first entry (the Pi starts from it)
truncate -s $(( (8192 + 2048 * FAT_MB + ROOT_KB * 2) * 512 )) "$X/head.img"
printf 'label: dos\nlabel-id: 0x%s\nunit: sectors\n\nstart=%s, size=%s, type=c\nstart=%s, size=%s, type=83\n' \
    "$DISKID" "$FAT_START" $((FAT_MB * 2048)) "$ROOT_AT" $((ROOT_KB * 2)) |
    sfdisk --wipe never -q "$X/head.img" >> "$L" 2>&1 || finish_ "RESULT: failed - table"
dd if="$X/head.img" of="$X/mbr.img" bs=4M count=1 2>> "$L" || finish_ "RESULT: failed - table copy"
rm -f "$X/head.img"
step img-write 12
say "writing $NAME ($LAYOUT)"
rm -f "$X/e2image.failed"
{
    cat "$X/mbr.img" || : > "$X/e2image.failed"
    if [ "$LAYOUT" = end ]; then
        e2image -ra "${DISK}p2" - 2>> "$L" || : > "$X/e2image.failed"
        cat "$F" || : > "$X/e2image.failed"
    else
        cat "$F" || : > "$X/e2image.failed"
        e2image -ra "${DISK}p2" - 2>> "$L" || : > "$X/e2image.failed"
    fi
} | $PACK > "$X/out/.$NAME" &
P=$!
# the bar: how much of the card the packer has read (12 % .. 98 %)
TOTAL=$(( 4194304 + ROOT_KB * 1024 + FAT_MB * 1048576 ))
while kill -0 "$P" 2>/dev/null; do
    sleep 5
    r=$(rchar "$P")
    case "$r" in ''|*[!0-9]*) continue ;; esac
    p=$(( 12 + r / (TOTAL / 86) ))
    [ "$p" -gt 98 ] && p=98
    progress "$p"
done
wait "$P"
rc=$?
rm -f "$F"
if [ $rc -ne 0 ] || [ -e "$X/e2image.failed" ]; then
    rm -f "$X/out/.$NAME"
    finish_ "RESULT: failed - writing the image (is the photo drive full?)"
fi
mv "$X/out/.$NAME" "$X/out/$NAME" || finish_ "RESULT: failed - renaming"
sync
finish_ "RESULT: ok $NAME"
IMAGE
}

u16() { od -An -tu2 -j"$2" -N2 "$1" | tr -d ' '; }
u32() { od -An -tu4 -j"$2" -N4 "$1" | tr -d ' '; }
p32() {
    v=$3
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $((v & 255)) $((v >> 8 & 255)) $((v >> 16 & 255)) $((v >> 24 & 255)))" |
        dd of="$1" bs=1 seek="$2" count=4 conv=notrunc 2>/dev/null
}

grow_fat() {
    # A card flashed from a Zero Ichi card image (zeroichi-card.img.gz): its ZEROICHI
    # drive is the last part of what was flashed, and its FAT tables were made
    # big enough for a 128 GB drive. At the first start it grows to the end of
    # the card: the partition, then the size in its boot sector - nothing is
    # copied, so it takes a moment. ($1 = the card, e.g. /dev/mmcblk0)
    D=$1
    d=${D#/dev/}
    [ -b "${D}p1" ] && [ -b "${D}p2" ] || return 0
    # (v52: the sizes read by the shell itself - no programs to load - and
    #  nothing more when the drive already reaches the end of the card: the
    #  usual case, at every start)
    s1="" n1="" s2="" size=""
    { read -r s1 < "/sys/block/$d/${d}p1/start"; read -r n1 < "/sys/block/$d/${d}p1/size"
      read -r s2 < "/sys/block/$d/${d}p2/start"; read -r size < "/sys/block/$d/size"; } 2>/dev/null
    for n in "$s1" "$n1" "$s2" "$size"; do
        case "$n" in ''|*[!0-9]*) return 0 ;; esac
    done
    [ "$s1" -gt "$s2" ] || return 0                       # (only that layout)
    [ -e "/sys/block/$d/${d}p3" ] && return 0
    [ $(((size - s1) / 8192 * 8192)) -gt $((n1 + 131072)) ] || return 0   # (no room to grow)
    [ "$(dd if="${D}p1" bs=1 skip=82 count=5 2>/dev/null)" = FAT32 ] || return 0
    spc=$(od -An -tu1 -j13 -N1 "${D}p1" | tr -d ' ')
    rsv=$(u16 "${D}p1" 14)
    nf=$(od -An -tu1 -j16 -N1 "${D}p1" | tr -d ' ')
    fsz=$(u32 "${D}p1" 36)
    tot=$(u32 "${D}p1" 32)
    for n in "$spc" "$rsv" "$nf" "$fsz" "$tot"; do
        case "$n" in ''|*[!0-9]*) return 0 ;; esac
    done
    [ "$spc" -gt 0 ] && [ "$tot" -le "$n1" ] || return 0
    max=$((rsv + nf * fsz + (fsz * 128 - 2) * spc))       # (what its FAT tables can hold)
    new=$((size - s1))
    [ "$new" -gt "$max" ] && new=$max
    new=$((new / 8192 * 8192))
    [ "$new" -gt $((n1 + 131072)) ] || return 0           # (64 MB more at least)
    mark "ZEROICHI drive: growing to the end of the card"
    echo ",$new" | sfdisk -N 1 --no-reread --wipe never --wipe-partitions never -q "$D" >/dev/null 2>&1
    n=0                                                   # (until the kernel has the new size)
    while [ "$(cat "/sys/block/$d/${d}p1/size" 2>/dev/null)" != "$new" ]; do
        n=$((n + 1))
        [ "$n" -gt 20 ] && return 0                        # (not this time: the next start)
        partx -u -n 1 "$D" >/dev/null 2>&1
        sleep 0.2
    done
    bk=$(u16 "${D}p1" 50)
    fsi=$(u16 "${D}p1" 48)
    p32 "${D}p1" 32 "$new"
    if [ "$bk" -gt 0 ]; then p32 "${D}p1" $((bk * 512 + 32)) "$new"; fi
    p32 "${D}p1" $((fsi * 512 + 488)) 4294967295        # (free space: worked out again)
    if [ "$bk" -gt 0 ]; then p32 "${D}p1" $(((bk + fsi) * 512 + 488)) 4294967295; fi
    sync
    mark "ZEROICHI drive: $((new / 2048)) MB"
}

register_devices() {
    # The camera library only uses video devices the device manager (udev) has
    # registered (a small database entry per device). The device manager takes
    # several seconds to start and work through its rules on a Pi Zero, so the
    # fast start doesn't run it: the camera's devices are registered directly.
    # (The camera helper registers any that turn up later the same way.)
    [ -d /run/udev/data ] || mkdir -p /run/udev/data
    read -r up _ < /proc/uptime
    for d in /sys/class/video4linux/* /sys/bus/media/devices/*; do
        [ -r "$d/dev" ] || continue
        read -r mm < "$d/dev"
        [ -e "/run/udev/data/c$mm" ] || echo "I:${up%.*}000000" > "/run/udev/data/c$mm"
    done
}

# Drivers the camera doesn't need: loaded after the camera's, on time the start
# leaves free (the video encoder - first used when you record or save - the old
# camera driver, temperature, i2c tools, USB...). Anything the camera's drivers
# need comes with them anyway.
# (LATER_DRIVERS until v51 - the list in devices() below:)
#   bcm2835_codec v4l2_mem2mem bcm2835_v4l2 rpivid_hevc rpi_hevc_dec raspberrypi_hwmon i2c_dev uio
#   uio_pdrv_genirq evdev bcm2835_rng rng_core joydev hid* usbhid dwc2 udc_core roles libcomposite
#   usb_f_* g_*

devices() {
    # The drivers this Pi uses (the list is made during a normal start-up):
    # the camera's first (the camera server waits for them), then the rest.
    # (the screen's SPI drivers: loaded by the main start-up, once the start-up
    # picture is on - it may use the SPI hardware directly meanwhile)
    # (v52: the two lists made by the shell itself - no grep to load; the same
    #  drivers as LATER_DRIVERS)
    now="" later=""
    if [ -f "$STATE/modules.list" ]; then
        while read -r m; do
            case "$m" in
                ''|'#'*|spi*) ;;
                bcm2835_codec|v4l2_mem2mem|bcm2835_v4l2|rpivid_hevc|rpi_hevc_dec|raspberrypi_hwmon|i2c_dev|uio|\
                uio_pdrv_genirq|evdev|bcm2835_rng|rng_core|joydev|hid*|usbhid|dwc2|udc_core|roles|libcomposite|\
                usb_f_*|g_*) later="$later $m" ;;
                *) now="$now $m" ;;
            esac
        done < "$STATE/modules.list"
    fi
    # shellcheck disable=SC2086
    modprobe -a -b $now >/dev/null 2>&1
    register_devices
    : > /run/zeroichi/devices-ready          # (the camera server stops waiting for more)
    mark "camera drivers loaded"
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        [ -w "$g" ] && echo performance > "$g"     # full speed (as while booting)
    done 2>/dev/null
    # (v53) the rest once the camera is ready (Zero Ichi says so: camera-ready -
    # at most 8 s): not while the camera server is still loading, which waits
    # for the processor the whole time. (An older zeroichi-lcd: at once.)
    [ -x "$LCD" ] && "$LCD" --wait /run/zeroichi/camera-ready 8 >/dev/null 2>&1
    if [ -n "$later" ]; then
        # shellcheck disable=SC2086
        nice -n 19 modprobe -a -b $later >/dev/null 2>&1
        register_devices
        mark "other drivers loaded"
    fi
    # swap in compressed RAM (like the normal start-up), for big photos - at
    # the lowest priority (v52: the camera is still loading meanwhile)
    if nice -n 19 modprobe zram >/dev/null 2>&1 && [ -e /sys/block/zram0/disksize ]; then
        mem=0
        while read -r k v _; do
            [ "$k" = "MemTotal:" ] && mem=$v && break
        done < /proc/meminfo
        echo lz4 > /sys/block/zram0/comp_algorithm 2>/dev/null
        if [ "$mem" -gt 0 ] && echo "$((mem / 2))K" > /sys/block/zram0/disksize 2>/dev/null; then
            nice -n 19 mkswap /dev/zram0 >/dev/null 2>&1 && nice -n 19 swapon -p 100 /dev/zram0 >/dev/null 2>&1
        fi
    fi
}

clock() {
    # The Pi has no clock battery: continue from the time saved at power-off
    # (v52: zeroichi-lcd does it - one small program instead of date and stat)
    c=""
    [ -x "$LCD" ] && c=$("$LCD" --clock /var/lib/zeroichi/clock /var/lib/systemd/timesync/clock \
        /etc/fake-hwclock.data 2>/dev/null)
    [ "$c" = ok ] && return 0
    now=$(date +%s)
    best=$now
    for f in /var/lib/zeroichi/clock /var/lib/systemd/timesync/clock; do
        t=$(stat -c %Y "$f" 2>/dev/null)
        [ -n "$t" ] && [ "$t" -gt "$best" ] && best=$t
    done
    if [ -f /etc/fake-hwclock.data ]; then
        t=$(date -u -d "$(cat /etc/fake-hwclock.data)" +%s 2>/dev/null)
        [ -n "$t" ] && [ "$t" -gt "$best" ] && best=$t
    fi
    [ "$best" -gt "$now" ] && date -s "@$best" >/dev/null 2>&1
}

# --- 1. the basics --------------------------------------------------------
# (v52) zeroichi-lcd mounts /proc and /sys and looks at KEY1, all in one tiny
# program - the start is waiting for the processor, and each "mount" takes a
# moment to load. An older zeroichi-lcd (no "ok ..."): the usual way.
EARLY=""
[ -x "$LCD" ] && EARLY=$("$LCD" --early "$KEY1" 2>/dev/null)
case "$EARLY" in
    "ok "*) ;;
    *)
        mount -t proc -o nosuid,nodev,noexec proc /proc >/dev/null 2>&1
        mount -t sysfs -o nosuid,nodev,noexec sysfs /sys >/dev/null 2>&1
        # (the kernel normally has /dev ready; the SD card's device shows that it is)
        [ -e /dev/mmcblk0 ] || mount -t devtmpfs -o mode=0755,nosuid devtmpfs /dev >/dev/null 2>&1
        ;;
esac

# normal start-up wanted (or no Python)? Also when cmdline.txt asks systemd to
# do something special (e.g. the repair script fixit.sh: "systemd.run=...")
read -r CMDLINE < /proc/cmdline 2>/dev/null
case " $CMDLINE " in
    *" systemd.run="* | *" systemd.unit="* | *" single "* | *" emergency "*) normal_start ;;
esac
if [ -e "$STATE/normal-start" ] || [ ! -x /usr/bin/python3 ] || key1_held; then
    normal_start
fi

# --- 2. file systems ------------------------------------------------------
# (v52: zeroichi-lcd again - /run, /dev/pts, /dev/shm, /tmp and the host name)
RUNOK=""
[ -x "$LCD" ] && RUNOK=$("$LCD" --mount-run 2>/dev/null)
if [ "$RUNOK" != ok ]; then
    mount -t tmpfs -o mode=0755,nosuid,nodev tmpfs /run >/dev/null 2>&1
    mkdir -p /run/lock /run/zeroichi /dev/pts /dev/shm
    mount -t devpts -o gid=5,mode=620 devpts /dev/pts >/dev/null 2>&1
    mount -t tmpfs -o nosuid,nodev tmpfs /dev/shm >/dev/null 2>&1
fi
mark "fast start began"

# the camera server first: loading the camera library is the slowest part of
# the start, so it loads while everything else starts up (Zero Ichi connects to
# it when it starts - see camd.py --listen). (v50: before anything else - it
# only reads, so the system partition needn't be writable yet - and ahead of
# the rest from its very first moment, not just once Python is running)
C=0                                        # the C program (if there is one)
[ -f "$APP/zeroichi-c" ] && [ -f "$APP/camd.py" ] && C=1
CAMD=""
if [ "$C" = 1 ]; then
    CAMD_PY=$APP/camd.py                   # (camd_start.py: its ready-compiled copy - quicker)
    [ -f "$APP/camd_start.py" ] && CAMD_PY=$APP/camd_start.py
    FIRST=""
    [ -x /usr/bin/nice ] && FIRST="/usr/bin/nice -n -2"
    $FIRST /usr/bin/python3 -S "$CAMD_PY" --listen </dev/null >/dev/null 2>>/run/zeroichi/errors.txt &
    CAMD=$!
    ZEROICHI_CAMD=$CAMD
    export ZEROICHI_CAMD
    mark "camera server started"
fi
# (v55: first, as until v53 - while the SD card has nothing else to do. v54
#  did it after the start-up picture, with the drivers already loading: it
#  then took the start-up script about a second - Zero Ichi started 0.6 s
#  later)
mount -o remount,rw / >/dev/null 2>&1 || normal_start
mark "system partition writable"
if [ "$C" = 1 ] && [ ! -x "$APP/zeroichi-c" ]; then
    chmod 755 "$APP/zeroichi-c" 2>/dev/null
    if [ ! -x "$APP/zeroichi-c" ]; then      # (can't be run after all: the Python Zero Ichi)
        kill "$CAMD" >/dev/null 2>&1
        CAMD=""
        unset ZEROICHI_CAMD
        C=0
    fi
fi

# --- 3. the drivers (in the background, the camera's first), the start-up picture
devices &
JOBS=$!
# The start-up picture at once (a tiny C program - Python takes seconds to
# load): straight through the SPI hardware, without waiting for its driver.
# Not possible? Then the usual way, once the driver is loaded. (Each Pi's own
# screen settings, and the picture that fits that screen - see zeroichi-lcd.c.)
# (v54: ahead of the camera server and the drivers for the processor - it was
#  shown a second after the fast start began, waiting for its turn; 0.6 s
#  sooner since. It only needs a few hundredths of a second of the
#  processor's time, which it would take before the camera is ready anyway.)
SPLASH="$APP/splash.rgb565 $APP/splash-320x240.rgb565 $STATE/splash.rgb565"
UP=""
[ -x /usr/bin/nice ] && UP="/usr/bin/nice -n -6"
if [ -x "$LCD" ] && [ -f "$STATE/lcd.conf" ]; then
    # shellcheck disable=SC2086
    if $UP "$LCD" --direct "$STATE/lcd.conf" $SPLASH 2>>/run/zeroichi/errors.txt; then
        : > /run/zeroichi/splash-ok
        : > /run/zeroichi/splash-direct
        mark "start-up picture shown"
    fi
fi
# (the rest of the basics: after the picture, so it shows a moment sooner)
if [ "$RUNOK" != ok ]; then
    mount -t tmpfs -o mode=1777,nosuid,nodev tmpfs /tmp >/dev/null 2>&1
    hostname -F /etc/hostname >/dev/null 2>&1
fi
clock
grow_fat /dev/mmcblk0                    # (a card just flashed from a Zero Ichi card image)
if [ -f /run/zeroichi/splash-ok ]; then
    modprobe -a -b spi_bcm2835 spidev >/dev/null 2>&1 &     # (Zero Ichi waits for it if need be)
    JOBS="$JOBS $!"
else
    modprobe -a -b spi_bcm2835 spidev >/dev/null 2>&1
    mark "screen driver loaded"
    # shellcheck disable=SC2086
    if [ -x "$LCD" ] && [ -f "$STATE/lcd.conf" ] && "$LCD" "$STATE/lcd.conf" $SPLASH 2>>/run/zeroichi/errors.txt; then
        : > /run/zeroichi/splash-ok
        mark "start-up picture shown"
    fi
fi
# (v53, once) Zero Ichi's files under its old name, ToyCam: the update moved
# everything still used to the new names (bootopt.py's migrate_names) - the
# rest goes now. The log carries on from the update's lines. The drive
# TOYCAM: renamed ZEROICHI just before it is opened (card_partitions).
if [ -d /var/lib/toycam ] || [ -e /usr/local/sbin/toycam-init ]; then
    rm -rf /var/lib/toycam /var/cache/toycam /usr/local/sbin/toycam-init \
        /usr/local/sbin/.toycam-init-old /usr/local/sbin/toycam-lcd >/dev/null 2>&1
    : > /run/zeroichi/log-continues
    mark "the old name's files removed"
fi
RELABEL=""
[ -f "$STATE/fat-relabel" ] && rm -f "$STATE/fat-relabel" && RELABEL=1
if [ -f "$APP/zeroichi.py" ] && [ ! -f "$STATE/space-format" ]; then
    card_partitions &                      # Zero Ichi's copy starts without them
    JOBS="$JOBS $!"
    PROG=$APP/zeroichi.py
elif [ -f "$APP/zeroichi.py" ]; then
    card_partitions                        # (once, after making room: it adds to /etc/fstab)
    PROG=$APP/zeroichi.py
else
    card_partitions
    PROG=$SD/zeroichi.py
fi

# --- 4. Zero Ichi --------------------------------------------------------------
cd / || true
quick=0
updated=0
cfails=0
while :; do
    read -r t0 _ < /proc/uptime
    if [ "$C" = 1 ]; then
        system_ro
        mark "Zero Ichi started"
        "$APP/zeroichi-c" </dev/null >/dev/null 2>>/run/zeroichi/errors.txt &
    else
        system_rw                          # (the Python Zero Ichi still writes its helper files)
        mark "Zero Ichi started (Python)"
        /usr/bin/python3 -S "$PROG" </dev/null >/dev/null 2>>/run/zeroichi/errors.txt &
    fi
    wait $!
    code=$?
    card_rw
    if [ -n "$CAMD" ]; then                # (the camera server started for the first Zero Ichi: ended)
        kill "$CAMD" 2>/dev/null
        wait "$CAMD" 2>/dev/null
        CAMD=""
        unset ZEROICHI_CAMD
    fi
    case $code in                          # (the system partition: writable for what follows)
        64) finish poweroff ;;
        65) system_rw; touch "$STATE/normal-start"; sync; finish reboot ;;
        66) finish reboot ;;
        67) system_rw
            if ! make_space; then          # (it restarts the Pi when it runs)
                echo "the plan was not usable - nothing was changed" >> "$STATE/space-log.txt"
                space_failed
            fi
            : > /run/zeroichi/space-tried            # (not again until the next start)
            mark "making room for photos was not possible (see space-log.txt)"
            continue ;;
        70) system_rw
            if ! make_image; then          # (it restarts the Pi when it runs)
                echo "RESULT: failed - the plan was not usable" >> "$STATE/image-log.txt"
            fi
            continue ;;
        68) system_rw
            if [ "$updated" = 0 ] && [ -f "$APP/zeroichi.py" ]; then
                updated=1                  # new files on the SD card: refresh the copy
                mark "refreshing Zero Ichi's copy"
                /usr/bin/python3 -S "$APP/zeroichi.py" --maintenance </dev/null >/dev/null \
                    2>>/run/zeroichi/errors.txt
                [ -f "$APP/zeroichi-c" ] && chmod 755 "$APP/zeroichi-c" 2>/dev/null
                [ -x "$APP/zeroichi-c" ] && [ -f "$APP/camd.py" ] || C=0
            else
                C=0                        # (asked twice: the Python Zero Ichi sorts it out)
                : > /run/zeroichi/log-continues
            fi
            continue ;;
        69) C=0
            mark "using the Python Zero Ichi"
            continue ;;
    esac
    if [ "$C" = 1 ]; then                  # the C program keeps stopping: Python instead
        cfails=$((cfails + 1))
        : > /run/zeroichi/log-continues      # (the next log carries on: keeps what happened)
        if [ "$cfails" -ge 2 ]; then
            C=0
            mark "Zero Ichi (C) stopped twice (code $code) - using the Python Zero Ichi"
            continue
        fi
    fi
    system_rw
    mark "Zero Ichi stopped (code $code) - starting it again"
    read -r t1 _ < /proc/uptime
    if [ "${t1%.*}" -lt $((${t0%.*} + 60)) ]; then
        quick=$((quick + 1))
    else
        quick=0
    fi
    if [ "$quick" -ge 4 ]; then              # keeps failing: use the normal start-up
        touch "$STATE/normal-start"
        sync
        finish reboot
    fi
    sleep 3
done
normal_start
