/* Zero Ichi - where photos go (the PHOTOS drive or bootfs) and "Photo space":
 * using the whole card for photos (carried out by the fast start). */
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include "app.h"
#include "gpio.h"
#include "lcd.h"
#include "util.h"

#define ZEROICHI_STATE "/var/lib/zeroichi"
#define PHOTOS_MOUNT "/media/photos"
#define BOOT_PHOTOS "/boot/firmware/photos"

static const char *photos_mount(void) {
    const char *e = getenv("ZEROICHI_PHOTOS_MOUNT");
    return e ? e : PHOTOS_MOUNT;
}
static const char *boot_photos(void) {
    const char *e = getenv("ZEROICHI_BOOT_PHOTOS");
    return e ? e : BOOT_PHOTOS;
}
static const char *state_dir(void) {
    const char *e = getenv("ZEROICHI_STATE_DIR");
    return e ? e : ZEROICHI_STATE;
}

static int fstab_has(const char *mp) {
    char *t = read_file("/etc/fstab", NULL);
    if (!t) return 0;
    int found = 0;
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n")) {
        while (*ln == ' ' || *ln == '\t') ln++;
        if (*ln == '#') continue;
        char a[256], b[256];
        if (sscanf(ln, "%255s %255s", a, b) == 2 && !strcmp(b, mp)) found = 1;
    }
    free(t);
    return found;
}

static int photos_mounted(void) {
    if (getenv("ZEROICHI_SIM_PHOTOS_MOUNTED")) return 1;
    return is_mount(photos_mount());
}

static int whole_card(void);
static const char *boot_dir(void);

/* MENU > Photo space is only shown while it can still do something */
int photo_space_needed(void) {
    if (getenv("ZEROICHI_SIM") && getenv("ZEROICHI_SIM_PLAN")) return 1;       /* (tests: a made-up card) */
    return !whole_card() && !fstab_has(photos_mount());
}

const char *photo_space_label(void) {
    int whole = whole_card();
    if (A.W < 300) return whole ? "All" : "More";
    return whole ? "Whole card" : "Get more";
}

static void gbs(long long n, char *out, size_t cap) { snprintf(out, cap, "%.1f GB", n / 1e9); }

static void report_space_log(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/space-log.txt", state_dir());
    char *t = read_file(p, NULL);
    if (!t) return;
    sys_write(1);
    unlink(p);
    sys_write(0);
    char lines[8192] = "";
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n"))
        if (*ln && strncmp(ln, "Pass ", 5) && strncmp(ln, "Partition #", 11) && strncmp(ln, "Re-reading", 10) &&
            !strstr(ln, " records ") && strlen(lines) + strlen(ln) + 4 < sizeof lines) {
            strcat(lines, "\n  ");
            strcat(lines, ln);
        }
    logi("Make room for photos:%s", lines);
    free(t);
    if (photos_mounted()) {
        char g[32], msg[64];
        gbs(total_bytes(photos_mount()), g, sizeof g);
        snprintf(msg, sizeof msg, "PHOTOS drive ready\n%s", g);
        show_toast(msg, 4);
    } else if (whole_card()) {
        char g[32], msg[64];
        gbs(free_bytes(boot_dir()), g, sizeof g);
        snprintf(msg, sizeof msg, "Whole card ready\n%s for photos", g);
        show_toast(msg, 4);
    } else {
        show_toast("Photo space: didn't work\n(see zeroichi-log.txt)", 4);
    }
}

static void move_old_photos(void) {
    DIR *d = opendir(boot_photos());
    if (!d) return;
    char **names = NULL;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[800];
        snprintf(p, sizeof p, "%s/%s", boot_photos(), e->d_name);
        struct stat st;
        if (stat(p, &st) || !S_ISREG(st.st_mode)) continue;
        names = realloc(names, sizeof(char *) * (n + 1));
        names[n++] = strdup(e->d_name);
    }
    closedir(d);
    if (!n) {
        free(names);
        return;
    }
    long long total = 0;
    for (int i = 0; i < n; i++) {
        char p[800];
        snprintf(p, sizeof p, "%s/%s", boot_photos(), names[i]);
        total += file_size(p);
    }
    char tg[32];
    gbs(total, tg, sizeof tg);
    logi("Moving %d files (%s) from %s to the PHOTOS drive", n, tg, boot_photos());
    int done = 0;
    sd_hold();                                     /* (the card writable meanwhile - see sd_init) */
    for (int i = 0; i < n; i++) {
        if (i % 3 == 0) {
            char sub[64];
            snprintf(sub, sizeof sub, "to the PHOTOS drive: %d of %d", i + 1, n);
            show_message("Moving your photos", sub, (Rgb){20, 20, 28});
        }
        char src[800], dst[800], tmp[820];
        snprintf(src, sizeof src, "%s/%s", boot_photos(), names[i]);
        snprintf(dst, sizeof dst, "%s/%s", photos_mount(), names[i]);
        snprintf(tmp, sizeof tmp, "%s/.%s.moving", photos_mount(), names[i]);
        if (file_exists(dst) && file_size(dst) == file_size(src)) {
            unlink(src);
        } else if (copy_file(src, tmp) == 0 && rename(tmp, dst) == 0) {
            unlink(src);
            done++;
        } else {
            logw("Could not move %s", names[i]);
            unlink(tmp);
        }
        free(names[i]);
    }
    free(names);
    sync_all();
    rmdir(boot_photos());
    sd_release();
    logi("Moved %d files to the PHOTOS drive", done);
    show_toast("Photos moved to the PHOTOS drive", 3);
}

static int expected(void) {
    char f[600];
    snprintf(f, sizeof f, "%s/space-format", state_dir());
    return fstab_has(photos_mount()) || (getenv("ZEROICHI_FASTINIT") && file_exists(f));
}

/* photo_dir "auto": the PHOTOS partition when the card has one, else bootfs */
void choose_photo_dir(void) {
    if (strcmp(A.photo_setting, "auto") && strcmp(A.photo_setting, BOOT_PHOTOS)) {
        snprintf(A.photo_dir, sizeof A.photo_dir, "%s", A.photo_setting);
        mkdir_p(A.photo_dir);
        return;
    }
    if (!photos_mounted() && expected()) {
        double t = mono();
        while (!photos_mounted() && expected() && mono() - t < 90) {
            if (mono() - t > 1.5) show_message("Opening the PHOTOS drive", "one moment...", (Rgb){20, 20, 28});
            msleep(250);
        }
    }
    if (photos_mounted()) {
        snprintf(A.photo_dir, sizeof A.photo_dir, "%s", photos_mount());
        move_old_photos();
    } else {
        snprintf(A.photo_dir, sizeof A.photo_dir, "%s", boot_photos());
        if (fstab_has(photos_mount())) logw("PHOTOS partition not mounted - saving to %s", boot_photos());
    }
    mkdir_p(A.photo_dir);
    report_space_log();
}

/* ------------------------------------------------------------- the plan */
/* How the card gets its room (carried out by the fast start, make_space):
 *   SPACE_MOVE   the system partition is made smaller and moved to the end of
 *                the card; the space becomes ONE big FAT drive (ZEROICHI) that
 *                takes over from bootfs with all its files
 *   SPACE_FAT    the same when the system is at the end already (a first try
 *                stopped half-way)
 *   SPACE_PHOTOS cards too small to copy the system: an extra PHOTOS drive */
enum { SPACE_MOVE = 1, SPACE_FAT, SPACE_PHOTOS };

typedef struct {
    int mode, minutes;
    char disk[64], part[64], diskid[16];
    long long p1_start, p1_size, root_start, root_size, root_kb, root_sectors, root_new_start;
    long long fat_start, fat_sectors, p3_start;
    long long gain, system;          /* bytes: the drive for photos, the system partition */
} Plan;

#define MIB4 8192LL                  /* 4 MiB in sectors */
#define GIB_S (2097152LL)            /* 1 GiB in sectors */

static int in_path(const char *tool) {
    const char *dirs[] = {"/usr/sbin", "/usr/bin", "/sbin", "/bin", "/usr/local/sbin", "/usr/local/bin"};
    for (unsigned i = 0; i < 6; i++) {
        char p[256];
        snprintf(p, sizeof p, "%s/%s", dirs[i], tool);
        if (access(p, X_OK) == 0) return 1;
    }
    return 0;
}

static const char *test_sys(void) { return getenv("ZEROICHI_TEST_SYS"); }   /* (tests: a made-up card) */

static long long sys_ll(const char *path) {
    char full[512];
    snprintf(full, sizeof full, "%s%s", test_sys() && path[0] == '/' && strncmp(path, "/var", 4) ? test_sys() : "", path);
    char *t = read_file(full, NULL);
    if (!t) return -1;
    long long v = atoll(t);
    free(t);
    return v;
}

static long long up4(long long s) { return (s + MIB4 - 1) / MIB4 * MIB4; }

static const char *boot_dir(void) {          /* bootfs (or the ZEROICHI drive) */
    static char d[256];
    snprintf(d, sizeof d, "%s", boot_photos());
    char *sl = strrchr(d, '/');
    if (sl && sl != d) *sl = 0;
    return d;
}

/* the whole card holds photos already: a PHOTOS drive, or a big start-up drive */
static int whole_card(void) {
    if (photos_mounted()) return 1;
    long long t = total_bytes(boot_dir());
    return t >= (2LL << 30);
}

static int space_fails(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/space-fails", state_dir());
    long long v = sys_ll(p);
    return v > 0 ? (int)v : 0;
}

static void fill_estimate(Plan *pl) {
    if (pl->mode == SPACE_MOVE) {                  /* (copying the system: ~9 MB/s on a Pi Zero) */
        double m = pl->system / 9e6 / 60 + 4;
        pl->minutes = (int)ceil(m / 5) * 5;
    } else {
        pl->minutes = pl->mode == SPACE_FAT ? 2 : 10;
    }
}

static int space_plan(Plan *pl, char *why, size_t cap) {
    memset(pl, 0, sizeof *pl);
    const char *sim = getenv("ZEROICHI_SIM") ? getenv("ZEROICHI_SIM_PLAN") : NULL;
    if (sim) {                                   /* (tests: a made-up 32 GB card) */
        *pl = (Plan){SPACE_MOVE, 0, "/dev/mmcblk0", "/dev/mmcblk0p2", "1a2b3c4d", 8192, 1048576, 1056768,
                     61277184, 4513792, 9027584, 53305344, 1056768, 52248576, 0, 0, 0};
        if (!strcmp(sim, "fat")) {
            pl->mode = SPACE_FAT;
            pl->root_start = pl->root_new_start;
            pl->root_size = pl->root_sectors;
        } else if (!strcmp(sim, "photos")) {
            pl->mode = SPACE_PHOTOS;
            pl->p3_start = up4(pl->root_start + pl->root_sectors);
        }
        pl->system = pl->root_sectors * 512;
        pl->gain = pl->mode == SPACE_PHOTOS ? (62333952 - pl->p3_start) * 512 : pl->fat_sectors * 512;
        fill_estimate(pl);
        return 0;
    }
    const char *ts = test_sys();
    char tp[512];
    if (!ts && (whole_card() || fstab_has(photos_mount()))) {
        snprintf(why, cap, "done");
        return -1;
    }
    if (!getenv("ZEROICHI_FASTINIT")) {
        snprintf(why, cap, "needs fast_start = yes");
        return -1;
    }
    if (geteuid() != 0 && !ts) {
        snprintf(why, cap, "needs to run as root");
        return -1;
    }
    snprintf(tp, sizeof tp, "%s/proc/cmdline", ts ? ts : "");
    char *cmd = read_file(tp, NULL);
    char *p = cmd ? strstr(cmd, "root=PARTUUID=") : NULL;
    if (!p || strlen(p) < 25 || strncmp(p + 22, "-02", 3)) {
        free(cmd);
        snprintf(why, cap, "unusual card layout");
        return -1;
    }
    snprintf(pl->diskid, sizeof pl->diskid, "%.8s", p + 14);
    free(cmd);
    for (char *c = pl->diskid; *c; c++)
        if (!strchr("0123456789abcdef", *c)) {
            snprintf(why, cap, "unusual card layout");
            return -1;
        }
    struct stat st;
    stat("/", &st);
    char link[128], real[256];
    snprintf(link, sizeof link, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
    if (ts) {
        snprintf(tp, sizeof tp, "%s/rootpart", ts);
        char *rp = read_file(tp, NULL);
        snprintf(real, sizeof real, "/x/%s", rp ? rp : "");
        free(rp);
        real[strcspn(real, "\n")] = 0;
    } else if (!realpath(link, real)) {
        snprintf(why, cap, "card not readable");
        return -1;
    }
    const char *part = strrchr(real, '/') + 1;
    if (strncmp(part, "mmcblk", 6) || strlen(part) != 9 || strcmp(part + 7, "p2")) {
        snprintf(why, cap, "unusual card layout");
        return -1;
    }
    char disk[16];
    snprintf(disk, sizeof disk, "%.7s", part);
    long long ps[9] = {0}, pn[9] = {0};
    int have[9] = {0};
    for (int i = 1; i <= 8; i++) {
        char f[128];
        snprintf(f, sizeof f, "/sys/block/%s/%sp%d/start", disk, disk, i);
        ps[i] = sys_ll(f);
        snprintf(f, sizeof f, "/sys/block/%s/%sp%d/size", disk, disk, i);
        pn[i] = sys_ll(f);
        have[i] = ps[i] >= 0 && pn[i] > 0;
    }
    char f[128];
    snprintf(f, sizeof f, "/sys/block/%s/size", disk);
    long long disk_size = sys_ll(f);
    struct statvfs v;
    if (!have[1] || !have[2] || disk_size <= 0 || statvfs("/", &v)) {
        snprintf(why, cap, "card not readable");
        return -1;
    }
    for (int i = 4; i <= 8; i++)
        if (have[i]) {
            snprintf(why, cap, "the card already has other partitions");
            return -1;
        }
    snprintf(pl->disk, sizeof pl->disk, "/dev/%s", disk);
    snprintf(pl->part, sizeof pl->part, "/dev/%s", part);
    pl->p1_start = ps[1];
    pl->p1_size = pn[1];
    pl->root_start = ps[2];
    pl->root_size = pn[2];
    long long p1_end = ps[1] + pn[1], gap = ps[2] - p1_end;
    int at_end = ps[2] + pn[2] + 2 * MIB4 >= disk_size;
    if (have[3] && !(ps[3] >= p1_end && ps[3] + pn[3] <= ps[2])) {   /* (one in the gap: a first try) */
        snprintf(why, cap, "the card already has other partitions");
        return -1;
    }
    /* the system partition's new size: what it holds now, a quarter more and
     * 1 GiB to spare - at least 4 GiB (whole 4 MiB) */
    long long used = (long long)(v.f_blocks - v.f_bfree) * v.f_frsize;
    if (ts) {
        snprintf(tp, sizeof tp, "%s/used", ts);
        used = sys_ll(tp + strlen(ts));
    }
    long long target = (long long)(used * 1.25) + (1LL << 30);
    if (target < (4LL << 30)) target = 4LL << 30;
    target = (target + (4LL << 20) - 1) / (4LL << 20) * (4LL << 20);
    if (gap >= 2 * GIB_S && at_end && pn[2] % 2 == 0) {
        pl->mode = SPACE_FAT;                    /* the system is at the end already */
        pl->root_kb = pn[2] / 2;
        pl->root_sectors = pn[2];
        pl->root_new_start = ps[2];
        pl->fat_start = up4(p1_end);
        pl->fat_sectors = ps[2] - pl->fat_start;
        pl->system = pn[2] * 512;
    } else if (gap < 64 * 2048 && !have[3]) {    /* as Raspberry Pi OS left it */
        if (target / 512 > pn[2]) target = pn[2] / MIB4 * MIB4 * 512;   /* (not grown: a flashed card image) */
        pl->root_kb = target / 1024;
        pl->root_sectors = target / 512;
        pl->system = target;
        pl->root_new_start = (disk_size - pl->root_sectors) / MIB4 * MIB4;
        pl->fat_start = up4(ps[2]);
        pl->fat_sectors = pl->root_new_start - pl->fat_start;
        if (pl->root_sectors <= pn[2] && pl->root_new_start >= ps[2] + pl->root_sectors &&
            pl->fat_sectors >= 2 * GIB_S) {
            pl->mode = SPACE_MOVE;
        } else {                                 /* too small to copy the system */
            pl->p3_start = up4(ps[2] + pl->root_sectors);
            if (pl->root_sectors > pn[2] || (disk_size - pl->p3_start) < 3 * GIB_S / 2) {
                snprintf(why, cap, "the card is too small");
                return -1;
            }
            pl->mode = SPACE_PHOTOS;
            pl->root_new_start = pl->fat_start = pl->fat_sectors = 0;
        }
    } else {
        snprintf(why, cap, "unusual card layout");
        return -1;
    }
    pl->gain = pl->mode == SPACE_PHOTOS ? (disk_size - pl->p3_start) * 512 : pl->fat_sectors * 512;
    if (pl->mode != SPACE_PHOTOS) {
        const char *iv = getenv("ZEROICHI_INIT");
        if (!iv || atoi(iv) < 3) {               /* (an older fast start is running: next start) */
            snprintf(why, cap, "please restart Zero Ichi first");
            return -1;
        }
    }
    const char *tools[] = {"e2fsck", "resize2fs", "sfdisk", "partx", "pivot_root", "chroot", "ldd", "mkfs.fat",
                           "blkid", "findfs", "dd", "cmp", "cp", "find"};
    char missing[256] = "";
    for (unsigned i = 0; i < sizeof tools / sizeof tools[0] && !ts; i++)
        if (!in_path(tools[i])) {
            strcat(missing, missing[0] ? ", " : "");
            strcat(missing, tools[i]);
        }
    if (missing[0]) {
        snprintf(why, cap, "missing tools: %s", missing);
        return -1;
    }
    fill_estimate(pl);
    return 0;
}

void test_space_plan(void) {                     /* (tests: ZEROICHI_TEST_SYS + ZEROICHI_TEST_PLAN) */
    Plan pl;
    char why[128] = "";
    if (space_plan(&pl, why, sizeof why)) {
        printf("NONE %s\n", why);
        return;
    }
    printf("%s P1_START=%lld P1_SIZE=%lld ROOT_START=%lld ROOT_SIZE=%lld ROOT_KB=%lld ROOT_SECTORS=%lld "
           "ROOT_NEW_START=%lld FAT_START=%lld FAT_SECTORS=%lld P3_START=%lld gain=%lld minutes=%d\n",
           pl.mode == SPACE_MOVE ? "move" : pl.mode == SPACE_FAT ? "fat" : "photos", pl.p1_start, pl.p1_size,
           pl.root_start, pl.root_size, pl.root_kb, pl.root_sectors, pl.root_new_start, pl.fat_start,
           pl.fat_sectors, pl.p3_start, pl.gain, pl.minutes);
}

static Plan offer;
static int offer_drawn, offer_auto;
static double offer_deadline;
#define AUTO_SECONDS 10

/* at start-up: the card gets its room by itself (after a short countdown) -
 * unless settings.txt says photo_space = menu, or two tries didn't work */
void maybe_offer_space(void) {
    if (strcmp(A.photo_setting, "auto") && strcmp(A.photo_setting, BOOT_PHOTOS)) return;
    if (!strcmp(A.S.photo_space, "menu") || space_fails() >= 2) return;
    if (file_exists("/run/zeroichi/space-tried")) return;
    char why[128];
    if (space_plan(&offer, why, sizeof why) == 0) {
        logi("Photo space: starting by itself in %d s (%s card)", AUTO_SECONDS,
             offer.mode == SPACE_PHOTOS ? "small" : "one-drive");
        offer_drawn = 0;
        offer_auto = 1;
        offer_deadline = mono() + AUTO_SECONDS;
        A.screen = SCR_SPACE;
    }
}

void photo_space_action(void) {
    char why[128];
    if (space_plan(&offer, why, sizeof why)) {
        if (!strcmp(why, "done")) {
            char g[32], msg[64];
            const char *dir = photos_mounted() ? photos_mount() : boot_dir();
            gbs(free_bytes(dir), g, sizeof g);
            snprintf(msg, sizeof msg, "Whole card in use\n%s free", g);
            show_toast(msg, 3);
        } else {
            logi("Photo space: not possible (%s)", why);
            char msg[160];
            snprintf(msg, sizeof msg, "Not possible:\n%s", why);
            show_toast(msg, 4);
        }
        return;
    }
    offer_drawn = 0;
    offer_auto = 0;
    A.screen = SCR_SPACE;
}

static void make_space(void);

void tick_space(void) {
    int left = 0;
    if (offer_auto) {
        left = (int)ceil(offer_deadline - mono());
        if (left <= 0) {
            make_space();
            return;
        }
        if (offer_drawn == left) return;
        offer_drawn = left;
    } else {
        if (offer_drawn) return;
        offer_drawn = 1;
    }
    char l1[64], l3[64];
    snprintf(l1, sizeof l1, "Takes about %d minutes.", offer.minutes);
    snprintf(l3, sizeof l3, "Starting in %d s", left);
    const char *lines[] = {"Use the whole card", "for photos", "", l1, "Your photos are kept.",
                           "Don't switch off meanwhile.", "", offer_auto ? l3 : "KEY1 = yes   KEY2 = not now",
                           "KEY1 = start  KEY2 = later"};
    screen_lines(lines, offer_auto ? 9 : 8, YELLOW, (Rgb){20, 20, 28});
    lcd_show(A.scr.px);
}

/* keep the screen's picture for the fast start (it puts it back after Zero Ichi
 * has stopped) */
static void save_screen_as(const char *path) {
    size_t n = (size_t)A.W * A.H;
    uint16_t *be = malloc(n * 2);
    for (size_t i = 0; i < n; i++) be[i] = __builtin_bswap16(A.scr.px[i]);
    write_file(path, be, n * 2, 1);
    free(be);
}

static void save_screen_message(void) {        /* (in RAM: /run) */
    char p[600];
    snprintf(p, sizeof p, "%s/message.rgb565", getenv("ZEROICHI_STATE_DIR") ? state_dir() : "/run/zeroichi");
    save_screen_as(p);
}

void save_message_screen(void) { save_screen_message(); }

/* the screens the fast start shows while it works, one per step */
static void make_step_screens(const Plan *pl) {
    static const char *const MOVE[][2] = {{"check", "checking the card"}, {"shrink", "making the system smaller"},
                                          {"move", "moving the system"}, {"drive", "making the ZEROICHI drive"}};
    static const char *const FAT[][2] = {{"drive", "making the ZEROICHI drive"}};
    static const char *const PHOTOS[][2] = {{"check", "checking the card"}, {"shrink", "making the system smaller"},
                                            {"parts", "adding the PHOTOS drive"}};
    const char *const(*steps)[2] = pl->mode == SPACE_MOVE ? MOVE : pl->mode == SPACE_FAT ? FAT : PHOTOS;
    int n = pl->mode == SPACE_MOVE ? 4 : pl->mode == SPACE_FAT ? 1 : 3;
    char dir[600];
    snprintf(dir, sizeof dir, "%s/steps", state_dir());
    mkdir_p(dir);
    for (int i = n - 1; i >= 0; i--) {             /* (the first one stays on the screen) */
        char l1[48], l4[64], path[700];
        snprintf(l1, sizeof l1, n > 1 ? "Step %d of %d:" : "One moment:", i + 1, n);
        snprintf(l4, sizeof l4, "About %d minutes in all.", pl->minutes);
        const char *lines[] = {"Making room", "for photos", "", l1, steps[i][1], "", "Don't switch off!",
                               "The green light blinks.", l4};
        screen_lines(lines, 9, YELLOW, (Rgb){20, 20, 28});
        snprintf(path, sizeof path, "%s/%s.rgb565", dir, steps[i][0]);
        save_screen_as(path);
    }
}

static void make_space(void) {
    Plan pl;
    char why[128];
    if (space_plan(&pl, why, sizeof why)) {
        A.screen = SCR_CAMERA;
        char msg[160];
        snprintf(msg, sizeof msg, "Not possible:\n%s", why);
        show_toast(msg, 4);
        return;
    }
    if (A.rec) stop_recording();
    wait_for_saves("Finishing saving...");
    if (A.developer.pending) {
        show_message("Finishing a video...", "one moment", (Rgb){20, 20, 28});
        worker_wait(&A.developer, 600);
        pump_results();
    }
    if (pl.mode == SPACE_PHOTOS)
        logi("Making room for photos: system partition %s -> %.1f GB, PHOTOS drive %.1f GB", pl.part,
             pl.system / 1e9, pl.gain / 1e9);
    else
        logi("Making room for photos: system partition %s -> %.1f GB at the end of the card, one ZEROICHI drive "
             "%.1f GB (%s)", pl.part, pl.system / 1e9, pl.gain / 1e9, pl.mode == SPACE_FAT ? "second part" : "all");
    sys_write(1);
    make_step_screens(&pl);
    lcd_show_now(A.scr.px);
    save_screen_message();
    char text[1024], p[600], tmp[620];
    int n = snprintf(text, sizeof text,
                     "MODE=%s\nDISK=%s\nPART=%s\nDISKID=%s\nP1_START=%lld\nP1_SIZE=%lld\nROOT_START=%lld\n"
                     "ROOT_SIZE=%lld\nROOT_KB=%lld\nROOT_SECTORS=%lld\n",
                     pl.mode == SPACE_MOVE ? "move" : pl.mode == SPACE_FAT ? "fat" : "photos", pl.disk, pl.part,
                     pl.diskid, pl.p1_start, pl.p1_size, pl.root_start, pl.root_size, pl.root_kb, pl.root_sectors);
    if (pl.mode == SPACE_PHOTOS)
        snprintf(text + n, sizeof text - n, "P3_START=%lld\n", pl.p3_start);
    else
        snprintf(text + n, sizeof text - n, "ROOT_NEW_START=%lld\nFAT_START=%lld\nFAT_SECTORS=%lld\n",
                 pl.root_new_start, pl.fat_start, pl.fat_sectors);
    snprintf(p, sizeof p, "%s/space-plan", state_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int bad = write_file(tmp, text, strlen(text), 1) || rename(tmp, p);
    sys_write(0);
    if (bad) {
        loge("Could not start making room");
        A.screen = SCR_CAMERA;
        show_toast("Photo space failed", 2);
        return;
    }
    A.st.space_asked = 1;
    save_state();
    cleanup(1);
    remember_clock();
    app_exit(EXIT_MAKE_SPACE);
}

void on_space(int b) {
    if (b == BTN_KEY1) {
        make_space();
        return;
    }
    if (b != BTN_KEY2) return;
    A.st.space_asked = 1;
    mark_dirty();
    A.screen = SCR_CAMERA;
    show_toast(offer_auto ? "Later: at the next start\n(or MENU > Photo space)" : "Later: MENU >\nPhoto space", 2.5);
    offer_auto = 0;
}

/* ------------------------------------------------------------- card image */
/* MAKE-IMAGE.txt in the zeroichi folder: the fast start writes a card image
 * (zeroichi-card.img.xz) onto the photo drive - flash it onto new cards for a
 * ready Zero Ichi (fast-start.sh, make_image) */
static int mount_dev(const char *mp, char *dev, size_t cap) {
    char *t = read_file("/proc/mounts", NULL);
    int ok = 0;
    if (!t) return 0;
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n")) {
        char a[256], b[256];
        if (sscanf(ln, "%255s %255s", a, b) == 2 && !strcmp(b, mp) && !strncmp(a, "/dev/", 5)) {
            snprintf(dev, cap, "%s", a);
            ok = 1;
        }
    }
    free(t);
    return ok;
}

static void image_report(void) {
    char p[600];
    snprintf(p, sizeof p, "%s/image-log.txt", state_dir());
    char *t = read_file(p, NULL);
    if (!t) return;
    sys_write(1);
    unlink(p);
    sys_write(0);
    const char *r = strstr(t, "RESULT: ok ");
    char done[96] = "Card image ready";
    if (r) snprintf(done, sizeof done, "Card image ready:\n%.*s", (int)strcspn(r + 11, "\n"), r + 11);
    char lines[6144] = "";
    for (char *ln = strtok(t, "\n"); ln; ln = strtok(NULL, "\n"))
        if (*ln && strncmp(ln, "Pass ", 5) && !strstr(ln, " records ") && strlen(lines) + strlen(ln) + 4 < sizeof lines) {
            strcat(lines, "\n  ");
            strcat(lines, ln);
        }
    logi("Card image:%s", lines);
    free(t);
    show_toast(r ? done : "Card image: didn't work\n(see zeroichi-log.txt)", 5);
}

static int image_plan(char *text, size_t cap, char *why, size_t wcap) {
    const char *iv = getenv("ZEROICHI_INIT");
    if (!getenv("ZEROICHI_FASTINIT") || !iv || atoi(iv) < 6) {
        snprintf(why, wcap, "needs the fast start (restart once more)");
        return -1;
    }
    if (!whole_card()) {
        snprintf(why, wcap, "set up the whole card first (Photo space)");
        return -1;
    }
    char out[256], boot[256];
    const char *outdir = photos_mounted() ? photos_mount() : boot_dir();
    if (!mount_dev(outdir, out, sizeof out) || !mount_dev(boot_dir(), boot, sizeof boot)) {
        snprintf(why, wcap, "drives not found");
        return -1;
    }
    char *cmd = read_file("/proc/cmdline", NULL);
    char *p = cmd ? strstr(cmd, "root=PARTUUID=") : NULL;
    char diskid[16] = "";
    if (p && strlen(p) >= 25 && !strncmp(p + 22, "-02", 3)) snprintf(diskid, sizeof diskid, "%.8s", p + 14);
    free(cmd);
    struct stat st;
    char link[128], real[256];
    stat("/", &st);
    snprintf(link, sizeof link, "/sys/dev/block/%u:%u", major(st.st_dev), minor(st.st_dev));
    const char *part = realpath(link, real) ? strrchr(real, '/') + 1 : "";
    if (!diskid[0] || strncmp(part, "mmcblk", 6) || strlen(part) != 9 || strcmp(part + 7, "p2")) {
        snprintf(why, wcap, "unusual card layout");
        return -1;
    }
    char disk[16], f[128];
    snprintf(disk, sizeof disk, "%.7s", part);
    snprintf(f, sizeof f, "/sys/block/%s/%s/start", disk, part);
    long long start = sys_ll(f);
    snprintf(f, sizeof f, "/sys/block/%s/%s/size", disk, part);
    long long size = sys_ll(f);
    struct statvfs v;
    if (start <= 0 || size <= 0 || statvfs("/", &v)) {
        snprintf(why, wcap, "card not readable");
        return -1;
    }
    long long used = (long long)(v.f_blocks - v.f_bfree) * v.f_frsize;
    /* usually the system goes first in the image, as small as is sensible, and
     * ZEROICHI after it (starting below 4 GB, where the Pi's start-up files are
     * sure to be found) - its FAT tables are made for a big drive, so a
     * flashed card only has to grow it (fast-start.sh: grow_fat) */
    const char *layout = "end";
    long long target = (long long)(used * 1.15) + (512LL << 20);
    target = (target + (4LL << 20) - 1) / (4LL << 20) * (4LL << 20);
    if (target > (3840LL << 20)) {               /* too big: ZEROICHI first, room made at the first start */
        layout = "start";
        target = (long long)(used * 1.25) + (1LL << 30);
        if (target < (4LL << 30)) target = 4LL << 30;
        target = (target + (4LL << 20) - 1) / (4LL << 20) * (4LL << 20);
    }
    if (target / 512 > size) target = size / MIB4 * MIB4 * 512;
    if (target < used + (256LL << 20)) {
        snprintf(why, wcap, "the system partition is too full");
        return -1;
    }
    if (free_bytes(outdir) < used + (2LL << 30)) {
        snprintf(why, wcap, "not enough free space on the card");
        return -1;
    }
    snprintf(text, cap,
             "DISK=/dev/%s\nDISKID=%s\nROOT_START=%lld\nROOT_SIZE=%lld\nROOT_KB=%lld\nOUT_DEV=%s\nBOOT_DEV=%s\nLAYOUT=%s\n",
             disk, diskid, start, size, target / 1024, out, boot, layout);
    return 0;
}

/* the screens the fast start shows while it makes the image (one per step),
 * and at the end - so the screen always says what is going on */
/* a line of text that fits the screen (a smaller size if need be) */
static void fit_text(int size, int y, const char *t, Rgb col) {
    while (size > 12 && cv_text_width(size, t) > A.W - 10) size -= 2;
    cv_text(&A.scr, size, A.W / 2, y, t, col, "mm", 1);
}

/* one step's screen: 3/5 of the way down stays free - the progress bar and
 * "N%" under it are drawn there by zeroichi-lcd (--progress) as the card image
 * is made */
static void image_step_screen(const char *step, const char *what, const char *note) {
    int wide = A.W >= 300;
    cv_fill(&A.scr, (Rgb){20, 20, 28});
    fit_text(wide ? 22 : 18, 24, "Making a card image", YELLOW);
    fit_text(wide ? 16 : 14, 58, step, (Rgb){200, 200, 215});
    fit_text(wide ? 18 : 16, 82, what, WHITE);
    if (note[0]) fit_text(14, 106, note, (Rgb){170, 170, 190});
    fit_text(wide ? 16 : 14, A.H - 36, "Don't switch off!", YELLOW);
    fit_text(14, A.H - 15, "It switches off when done.", (Rgb){190, 190, 210});
}

static void make_image_screens(void) {
    static const char *const STEP[][3] = {{"img-check", "checking the card", ""},
                                          {"img-slim", "leaving out the extras", ""},
                                          {"img-write", "writing the card image", "the longest part"}};
    char dir[600], path[700];
    snprintf(dir, sizeof dir, "%s/steps", state_dir());
    mkdir_p(dir);
    for (int i = 0; i < 3; i++) {
        char l1[32];
        snprintf(l1, sizeof l1, "Step %d of 3", i + 1);
        image_step_screen(l1, STEP[i][1], STEP[i][2]);
        snprintf(path, sizeof path, "%s/%s.rgb565", dir, STEP[i][0]);
        save_screen_as(path);
    }
    const char *done[] = {"Card image", "ready!", "", "zeroichi-card.img.xz", "is on the ZEROICHI drive.", "",
                          "Switching off now...", "Unplug when the green", "light is off."};
    screen_lines(done, 9, (Rgb){120, 230, 120}, (Rgb){20, 20, 28});
    snprintf(path, sizeof path, "%s/img-done.rgb565", dir);
    save_screen_as(path);
    const char *fail[] = {"Card image:", "didn't work", "", "Your photos and settings", "are fine.", "",
                          "Restarting now...", "(see zeroichi-log.txt)"};
    screen_lines(fail, 8, (Rgb){255, 120, 120}, (Rgb){20, 20, 28});
    snprintf(path, sizeof path, "%s/img-fail.rgb565", dir);
    save_screen_as(path);
}

void maybe_make_image(const char *sd_dir) {
    image_report();
    if (getenv("ZEROICHI_SIM_IMAGE_SCREENS")) {        /* (tests: just make the screens) */
        make_image_screens();
        return;
    }
    /* any file called MAKE-IMAGE... (Windows may add a hidden ".txt") */
    char trig[600] = "";
    DIR *d = opendir(sd_dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)))
            if (!strncasecmp(e->d_name, "MAKE-IMAGE", 10)) snprintf(trig, sizeof trig, "%s/%s", sd_dir, e->d_name);
        closedir(d);
    }
    if (!trig[0]) return;
    char text[1024], why[128];
    int r = image_plan(text, sizeof text, why, sizeof why);
    if (r && strstr(why, "restart once more")) {   /* (just updated: the new fast start runs next time) */
        logi("Card image: at the next start (%s)", why);
        show_toast("Card image:\nat the next start", 3);
        return;
    }
    sd_unlink(trig);                               /* (once - put it back to try again) */
    sync_all();
    if (r) {
        logw("Card image: not possible (%s)", why);
        char msg[160];
        snprintf(msg, sizeof msg, "Card image not possible:\n%s", why);
        show_toast(msg, 5);
        return;
    }
    logi("Card image: starting (%s)", text);
    sys_write(1);
    make_image_screens();
    const char *lines[] = {"Making a", "card image", "", "zeroichi-card.img.xz", "on the ZEROICHI drive.",
                           "About 30-60 minutes.", "Don't switch off!", "It switches off when done."};
    screen_lines(lines, 8, YELLOW, (Rgb){20, 20, 28});
    lcd_show_now(A.scr.px);
    save_screen_message();
    char p[600], tmp[620];
    snprintf(p, sizeof p, "%s/image-plan", state_dir());
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    int bad = write_file(tmp, text, strlen(text), 1) || rename(tmp, p);
    sys_write(0);
    if (bad) {
        loge("Card image: could not start");
        return;
    }
    cleanup(1);
    remember_clock();
    app_exit(EXIT_MAKE_IMAGE);
}
