#include "state.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include "looks.h"
#include "util.h"

/* --------------------------------------------------------------- settings */
static const char *const DISPLAYS[] = {"1.3inch-hat", "2.8inch-hat", "st7789-320x240", "ili9341-320x240"};

static void lower(char *s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static char *strip(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static int yes(const char *v) {
    return !strcasecmp(v, "1") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcasecmp(v, "on");
}

/* TextEdit "rich text": drop the formatting codes (like config.py) */
static void strip_rtf(char *t) {
    char *o = t;
    for (char *p = t; *p;) {
        if (*p == '\\' && (p[1] == '\n' || p[1] == '\r')) {
            *o++ = '\n';
            p += 2;
        } else if (*p == '\\' && !strncmp(p, "\\par", 4) && !isalpha((unsigned char)p[4 + (p[4] == 'd')])) {
            *o++ = '\n';
            p += 4 + (p[4] == 'd');
        } else if (*p == '\\' && p[1] == '\'') {
            p += 4;
        } else if (*p == '\\' && isalpha((unsigned char)p[1])) {
            p++;
            while (isalpha((unsigned char)*p)) p++;
            if (*p == '-') p++;
            while (isdigit((unsigned char)*p)) p++;
            if (*p == ' ') p++;
        } else if (*p == '{' || *p == '}') {
            p++;
        } else {
            *o++ = *p++;
        }
    }
    *o = 0;
}

void display_canon(char *name, size_t cap) {
    char v[64];
    snprintf(v, sizeof v, "%s", name);
    char *q = strip(v);
    lower(q);
    size_t n = strlen(q);
    static const char *const alias[][2] = {
        {"2.8", "2.8inch-hat"}, {"2.8inch", "2.8inch-hat"}, {"2.8inch hat", "2.8inch-hat"},
        {"1.3", "1.3inch-hat"}, {"1.3inch", "1.3inch-hat"}, {"1.3inch hat", "1.3inch-hat"},
        {"waveshare", "1.3inch-hat"}, {"hat", "1.3inch-hat"}, {"st7789", "1.3inch-hat"},
        {"ili9341", "ili9341-320x240"}};
    /* (older Zero Ichi versions called the 2.8inch hat "...-plus" / "...+") */
    if (n && (q[n - 1] == '+' || (n >= 4 && !strcmp(q + n - 4, "plus")))) {
        q = (char *)"2.8inch-hat";
    } else {
        for (unsigned i = 0; i < sizeof alias / sizeof alias[0]; i++)
            if (!strcmp(q, alias[i][0])) q = (char *)alias[i][1];
    }
    snprintf(name, cap, "%s", q);
}

void settings_read(const char *path, Settings *s) {
    memset(s, 0, sizeof *s);
    strcpy(s->photo_dir, "auto");
    s->allow_shutdown = s->optimize_boot = s->fast_start = 1;
    s->spi_speed = 40000000;
    strcpy(s->photo_space, "auto");
    strcpy(s->display, "1.3inch-hat");
    s->display_madctl = s->display_invert = s->spi_cs = -1;
    s->display_bits = 16;
    s->pin_dc = s->pin_rst = s->pin_bl = -1;
    for (int i = 0; i < 8; i++) s->pin_key[i] = -1;
    strcpy(s->app, "c");
    size_t n;
    char *text = read_file(path, &n);
    if (!text) return;
    char *t = text;
    if (!strncmp(t, "\xef\xbb\xbf", 3)) t += 3;
    if (!strncmp(strip(t), "{\\rtf", 5)) strip_rtf(t);
    static const char *const keynames[8] = {"pin_up", "pin_down", "pin_left", "pin_right", "pin_press",
                                            "pin_key1", "pin_key2", "pin_key3"};
    for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = strip(line), *val = strip(eq + 1);
        for (char *k = key; *k; k++) if (*k == '-') *k = '_';
        lower(key);
        if (!*val || !strcasecmp(val, "auto") || !strcasecmp(val, "default")) continue;
        long iv = strtol(val, NULL, 0);
        if (!strcmp(key, "photo_dir")) snprintf(s->photo_dir, sizeof s->photo_dir, "%s", val);
        else if (!strcmp(key, "rotate")) s->rotate = (int)iv;
        else if (!strcmp(key, "swap_rb")) s->swap_rb = yes(val);
        else if (!strcmp(key, "allow_shutdown")) s->allow_shutdown = yes(val);
        else if (!strcmp(key, "no_camera")) s->no_camera = yes(val);
        else if (!strcmp(key, "diagnostics")) s->diagnostics = yes(val);
        else if (!strcmp(key, "optimize_boot")) s->optimize_boot = yes(val);
        else if (!strcmp(key, "fast_start")) s->fast_start = yes(val);
        else if (!strcmp(key, "spi_speed")) s->spi_speed = (int)iv;
        else if (!strcmp(key, "display_madctl")) s->display_madctl = (int)iv;
        else if (!strcmp(key, "display_invert")) s->display_invert = yes(val);
        else if (!strcmp(key, "spi_cs")) s->spi_cs = (int)iv;
        else if (!strcmp(key, "display_bits")) s->display_bits = (int)iv;
        else if (!strcmp(key, "pin_dc")) s->pin_dc = (int)iv;
        else if (!strcmp(key, "pin_rst")) s->pin_rst = (int)iv;
        else if (!strcmp(key, "pin_bl")) s->pin_bl = (int)iv;
        else if (!strcmp(key, "app")) { snprintf(s->app, sizeof s->app, "%s", val); lower(s->app); }
        else if (!strcmp(key, "photo_space")) {
            snprintf(s->photo_space, sizeof s->photo_space, "%s", val);
            lower(s->photo_space);
        }
        else if (!strcmp(key, "live_fps")) s->live_fps = (int)iv;
        else if (!strcmp(key, "display")) {
            char v[64];
            snprintf(v, sizeof v, "%s", val);
            char *q = v;
            if (*q == '"') q++;
            if (*q && q[strlen(q) - 1] == '"') q[strlen(q) - 1] = 0;
            snprintf(s->display, sizeof s->display, "%s", q);
            display_canon(s->display, sizeof s->display);
        } else {
            for (int i = 0; i < 8; i++)
                if (!strcmp(key, keynames[i])) s->pin_key[i] = (int)iv;
        }
    }
    free(text);
    int ok = 0;
    for (int i = 0; i < 4; i++) ok |= !strcmp(s->display, DISPLAYS[i]);
    if (!ok) strcpy(s->display, "1.3inch-hat");
    if (s->rotate != 0 && s->rotate != 180) s->rotate = 0;
}

/* ------------------------------------------------------------ tiny JSON */
typedef struct { const char *p; } J;
enum { JNONE, JSTR, JNUM, JBOOL, JNULL, JOBJ, JOTHER };

static void ws(J *j) { while (*j->p && isspace((unsigned char)*j->p)) j->p++; }

static int jstr(J *j, char *out, size_t cap) {
    ws(j);
    if (*j->p != '"') return -1;
    j->p++;
    size_t n = 0;
    while (*j->p && *j->p != '"') {
        int c = (unsigned char)*j->p++;
        if (c == '\\') {
            c = (unsigned char)*j->p++;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') {                        /* ° etc: keep as UTF-8 */
                unsigned u = (unsigned)strtoul((char[5]){j->p[0], j->p[1], j->p[2], j->p[3], 0}, NULL, 16);
                j->p += 4;
                if (u >= 0x800 && n + 3 < cap) {
                    out[n++] = (char)(0xE0 | (u >> 12));
                    out[n++] = (char)(0x80 | ((u >> 6) & 63));
                    c = 0x80 | (u & 63);
                } else if (u >= 0x80 && n + 2 < cap) {
                    out[n++] = (char)(0xC0 | (u >> 6));
                    c = 0x80 | (u & 63);
                } else {
                    c = (int)u;
                }
            }
        }
        if (n + 1 < cap) out[n++] = (char)c;
    }
    if (*j->p == '"') j->p++;
    out[n] = 0;
    return 0;
}

static void jskip(J *j);

/* one value: returns its kind; strings into s, numbers into *num */
static int jvalue(J *j, char *s, size_t cap, double *num) {
    ws(j);
    char c = *j->p;
    if (c == '"') {
        jstr(j, s, cap);
        return JSTR;
    }
    if (c == '{' || c == '[') {
        jskip(j);
        return c == '{' ? JOBJ : JOTHER;
    }
    if (!strncmp(j->p, "true", 4)) { j->p += 4; *num = 1; return JBOOL; }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; *num = 0; return JBOOL; }
    if (!strncmp(j->p, "null", 4)) { j->p += 4; return JNULL; }
    char *e;
    *num = strtod(j->p, &e);
    if (e == j->p) { j->p++; return JOTHER; }
    j->p = e;
    return JNUM;
}

static void jskip(J *j) {
    ws(j);
    char open = *j->p, close = open == '{' ? '}' : ']';
    if (open != '{' && open != '[') {
        char s[8];
        double d;
        jvalue(j, s, sizeof s, &d);
        return;
    }
    int depth = 0;
    while (*j->p) {
        if (*j->p == '"') {
            char s[2];
            jstr(j, s, sizeof s);
            continue;
        }
        if (*j->p == open) depth++;
        if (*j->p == close && --depth == 0) {
            j->p++;
            return;
        }
        j->p++;
    }
}

/* ------------------------------------------------------------------ state */
static const char *const SIZES[] = {"Screen", "4:3", "Square"};

/* (0.1.0) MENU > Image ratio (the setting "size"): what it shows and what a
 * preset says - "Fit screen" (the screen's shape), "4:3", "1:1" */
const char *ratio_label(const char *size) {
    return !strcmp(size, "Screen") ? "Fit screen" : !strcmp(size, "Square") ? "1:1" : size;
}

static int ratio_pick(const char *v, char *out, size_t cap) {
    static const char *const NAMES[][2] = {{"Screen", "Screen"}, {"Fit screen", "Screen"}, {"Fit", "Screen"},
                                           {"4:3", "4:3"}, {"Square", "Square"}, {"1:1", "Square"}};
    for (unsigned i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++)
        if (!strcasecmp(v, NAMES[i][0])) {
            snprintf(out, cap, "%s", NAMES[i][1]);
            return 1;
        }
    return 0;
}
const char *const CLIP_NAMES[2] = {"320x240", "640x480"};
static const char *const SAVERS[] = {"Off", "30 s", "1 min", "2 min", "5 min"};

/* (v55) only two clip sizes: the older ones become the nearer of the two
 * (and "Small" / "Medium" / "Large" from before v47) */
static void clip_canon(char *v, size_t cap) {
    static const char *const SMALL[] = {"Small", "small", "384x288", "448x336"};
    static const char *const BIG[] = {"Medium", "medium", "Large", "large", "512x384", "576x432"};
    for (unsigned i = 0; i < sizeof SMALL / sizeof SMALL[0]; i++)
        if (!strcmp(v, SMALL[i])) snprintf(v, cap, "%s", "320x240");
    for (unsigned i = 0; i < sizeof BIG / sizeof BIG[0]; i++)
        if (!strcmp(v, BIG[i])) snprintf(v, cap, "%s", "640x480");
}

static int in_list(const char *v, const char *const *l, int n) {
    for (int i = 0; i < n; i++)
        if (!strcmp(v, l[i])) return 1;
    return 0;
}

void state_default(State *st, const Settings *s) {
    memset(st, 0, sizeof *st);
    strcpy(st->mode, "PHOTO");
    st->filter = 0;
    st->ev = 0;
    st->screen_flip = s && s->rotate == 180;
    st->gif_frames = 16;
    st->video_max = 30;
    strcpy(st->size, "Screen");
    strcpy(st->clip_size, "640x480");
    strcpy(st->screensaver, "1 min");
    st->next_number = 1;
    strcpy(st->key3, "Power off");
}

static const char *const OLD_ORDER[] = {"Normal", "Harinezumi", "8mm", "Holga B&W", "Mono", "Sepia", "Lomo",
                                        "Cross", "GameBoy", "8-Bit", "Thermal", "Negative"};

/* merge one saved state (JSON text) into st (Zero Ichi.merge_state) */
static void merge(State *st, const char *text) {
    J j = {text};
    ws(&j);
    if (*j.p != '{') return;
    j.p++;
    char key[64], sval[128], filter_name[64] = "", pixel_palette[32] = "";
    int old_filter = -1, have_name = 0;
    for (;;) {
        ws(&j);
        if (*j.p == '}' || !*j.p) break;
        if (*j.p == ',') { j.p++; continue; }
        if (jstr(&j, key, sizeof key)) break;
        ws(&j);
        if (*j.p == ':') j.p++;
        ws(&j);
        double num = 0;
        if (!strcmp(key, "screens") && *j.p == '{') {    /* {"pi serial": "1.3inch-hat", ...} */
            j.p++;
            st->nscreens = 0;
            for (;;) {
                ws(&j);
                if (*j.p == '}' || !*j.p) { if (*j.p) j.p++; break; }
                if (*j.p == ',') { j.p++; continue; }
                char k2[40];
                if (jstr(&j, k2, sizeof k2)) break;
                ws(&j);
                if (*j.p == ':') j.p++;
                int kind = jvalue(&j, sval, sizeof sval, &num);
                if (kind == JSTR && st->nscreens < 8 && k2[0]) {
                    char nm[32];
                    snprintf(nm, sizeof nm, "%s", sval);
                    display_canon(nm, sizeof nm);
                    if (in_list(nm, DISPLAYS, 4)) state_set_screen_of(st, k2, nm);
                }
            }
            continue;
        }
        if (!strcmp(key, "space") && *j.p == '{') {      /* {"photo:Normal": 0.12, ...} */
            j.p++;
            st->nspace = 0;
            for (;;) {
                ws(&j);
                if (*j.p == '}' || !*j.p) { if (*j.p) j.p++; break; }
                if (*j.p == ',') { j.p++; continue; }
                char k2[40];
                if (jstr(&j, k2, sizeof k2)) break;
                ws(&j);
                if (*j.p == ':') j.p++;
                int kind = jvalue(&j, sval, sizeof sval, &num);
                if (kind == JNUM && st->nspace < NSPACE) {
                    snprintf(st->space_key[st->nspace], sizeof st->space_key[0], "%s", k2);
                    st->space_val[st->nspace++] = num;
                }
            }
            continue;
        }
        int kind = jvalue(&j, sval, sizeof sval, &num);
        int b = kind == JBOOL, n = kind == JNUM, s = kind == JSTR;
        if (!strcmp(key, "mode") && s) snprintf(st->mode, sizeof st->mode, "%s", sval);
        else if (!strcmp(key, "filter_name") && s) { snprintf(filter_name, sizeof filter_name, "%s", sval); have_name = 1; }
        else if (!strcmp(key, "pixel_palette") && s) snprintf(pixel_palette, sizeof pixel_palette, "%s", sval);
        else if (!strcmp(key, "filter") && n) old_filter = (int)num;
        else if (!strcmp(key, "ev") && n) st->ev = num < -2 ? -2 : num > 2 ? 2 : num;
        else if (!strcmp(key, "cam_rotate") && n) st->cam_rotate = (int)num;
        else if (!strcmp(key, "mirror") && b) st->mirror = (int)num;
        else if (!strcmp(key, "screen_flip") && b) st->screen_flip = (int)num;
        else if (!strcmp(key, "gif_frames") && n) st->gif_frames = (int)num;
        else if (!strcmp(key, "video_max") && n) st->video_max = (int)num;
        else if (!strcmp(key, "size") && s) snprintf(st->size, sizeof st->size, "%s", sval);
        else if (!strcmp(key, "clip_size") && s) snprintf(st->clip_size, sizeof st->clip_size, "%s", sval);
        else if (!strcmp(key, "screen") && s) {
            snprintf(st->screen, sizeof st->screen, "%s", sval);
            if (st->screen[0]) display_canon(st->screen, sizeof st->screen);
        }
        else if (!strcmp(key, "screensaver") && s) snprintf(st->screensaver, sizeof st->screensaver, "%s", sval);
        else if (!strcmp(key, "next_number") && n) st->next_number = (int)num;
        else if (!strcmp(key, "space_asked") && b) st->space_asked = (int)num;
        else if (!strcmp(key, "key3") && s) snprintf(st->key3, sizeof st->key3, "%s", sval);
    }
    /* filters: renamed / older versions (see merge_state) */
    if (have_name && (!strcmp(filter_name, "16-Bit") || !strcmp(filter_name, "8-Bit")) && pixel_palette[0]) {
        char n2[64];
        snprintf(n2, sizeof n2, "16-Bit %s", pixel_palette);
        if (look_by_name(n2) >= 0) snprintf(filter_name, sizeof filter_name, "%s", n2);
    }
    if (!have_name && old_filter >= 0) {
        const char *nm = old_filter < 12 ? OLD_ORDER[old_filter] : "Normal";
        snprintf(filter_name, sizeof filter_name, "%s", nm);
        have_name = 1;
    }
    if (have_name) {
        int id = look_by_name(filter_name);
        if (id >= 0) st->filter = id;
    }
}

void state_validate(State *st) {
    clip_canon(st->clip_size, sizeof st->clip_size);
    State d;
    state_default(&d, NULL);
    if (st->cam_rotate != 0 && st->cam_rotate != 90 && st->cam_rotate != 180 && st->cam_rotate != 270)
        st->cam_rotate = 0;
    if (!in_list(st->size, SIZES, 3)) strcpy(st->size, d.size);
    if (!in_list(st->clip_size, CLIP_NAMES, 2)) strcpy(st->clip_size, d.clip_size);
    if (st->gif_frames != 8 && st->gif_frames != 16 && st->gif_frames != 24) st->gif_frames = 16;
    {
        static const int OK[] = {15, 30, 60, 120, 300, 600, 720};    /* (12 minutes at most) */
        int ok = 0;
        for (unsigned i = 0; i < sizeof OK / sizeof OK[0]; i++) ok |= st->video_max == OK[i];
        if (!ok) st->video_max = st->video_max <= 0 || st->video_max > 720 ? 720 : 30;
    }
    if (!in_list(st->screensaver, SAVERS, 5)) strcpy(st->screensaver, d.screensaver);
    if (strcmp(st->key3, "Power off") && strcmp(st->key3, "Standby")) strcpy(st->key3, "Power off");
    if (st->screen[0] && !in_list(st->screen, DISPLAYS, 4)) st->screen[0] = 0;
    if (strcmp(st->mode, "PHOTO") && strcmp(st->mode, "GIF") && strcmp(st->mode, "VIDEO")) strcpy(st->mode, "PHOTO");
    st->filter = ((st->filter % NLOOKS) + NLOOKS) % NLOOKS;
    if (st->next_number < 1) st->next_number = 1;
}

int state_load(State *st, const char *path) {
    char *t = path ? read_file(path, NULL) : NULL;
    const char *q = t;
    while (q && isspace((unsigned char)*q)) q++;
    int ok = q && *q == '{';
    if (ok) merge(st, t);
    state_validate(st);
    free(t);
    return ok;
}

static void jstr_out(Buf *b, const char *s) {
    buf_u8(b, '"');
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') buf_u8(b, '\\');
        buf_u8(b, *s);
    }
    buf_u8(b, '"');
}

int state_save(const State *st, const char *path) {
    Buf b = {0};
    char tmp[256];
    struct timeval tv;
    gettimeofday(&tv, NULL);
#define KEY(k) do { buf_add(&b, " \"" k "\": ", strlen(k) + 5); } while (0)
#define STR(k, v) do { KEY(k); jstr_out(&b, v); buf_add(&b, ",\n", 2); } while (0)
#define NUM(k, fmt, v) do { KEY(k); snprintf(tmp, sizeof tmp, fmt, v); buf_add(&b, tmp, strlen(tmp)); buf_add(&b, ",\n", 2); } while (0)
#define BOOL(k, v) do { KEY(k); buf_add(&b, (v) ? "true" : "false", (v) ? 4 : 5); buf_add(&b, ",\n", 2); } while (0)
    buf_add(&b, "{\n", 2);
    STR("mode", st->mode);
    NUM("ev", "%.1f", st->ev);
    NUM("cam_rotate", "%d", st->cam_rotate);
    BOOL("mirror", st->mirror);
    BOOL("screen_flip", st->screen_flip);
    NUM("gif_frames", "%d", st->gif_frames);
    NUM("video_max", "%d", st->video_max);
    STR("size", st->size);
    STR("clip_size", st->clip_size);
    STR("screen", st->screen);
    STR("screensaver", st->screensaver);
    KEY("space");
    buf_add(&b, "{", 1);
    for (int i = 0; i < st->nspace; i++) {
        buf_add(&b, i ? ",\n  " : "\n  ", i ? 4 : 3);
        jstr_out(&b, st->space_key[i]);
        snprintf(tmp, sizeof tmp, ": %.6g", st->space_val[i]);
        buf_add(&b, tmp, strlen(tmp));
    }
    buf_add(&b, st->nspace ? "\n },\n" : "},\n", st->nspace ? 5 : 3);
    KEY("screens");
    buf_add(&b, "{", 1);
    for (int i = 0; i < st->nscreens; i++) {
        buf_add(&b, i ? ", " : "", i ? 2 : 0);
        jstr_out(&b, st->scr_id[i]);
        buf_add(&b, ": ", 2);
        jstr_out(&b, st->scr_name[i]);
    }
    buf_add(&b, "},\n", 3);
    NUM("next_number", "%d", st->next_number);
    BOOL("space_asked", st->space_asked);
    STR("key3", st->key3);
    NUM("_saved", "%.3f", tv.tv_sec + tv.tv_usec / 1e6);
    KEY("filter_name");
    jstr_out(&b, LOOK_NAMES[st->filter]);
    buf_add(&b, "\n}", 2);
    int bad = 0;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = 0;
        mkdir_p(dir);
    }
    if (write_file_atomic(path, b.p, b.n)) {
        logw("Could not save settings to %s", path);
        bad = 1;
    }
    buf_free(&b);
    sync_all();
    return bad ? -1 : 0;
}

int state_equal(const State *a, const State *b) { return !memcmp(a, b, sizeof *a); }

const char *state_screen_of(const State *st, const char *pi) {
    if (!pi || !pi[0]) return NULL;
    for (int i = 0; i < st->nscreens; i++)
        if (!strcmp(st->scr_id[i], pi)) return st->scr_name[i];
    return NULL;
}

void state_set_screen_of(State *st, const char *pi, const char *name) {
    if (!pi || !pi[0]) return;
    int i;
    for (i = 0; i < st->nscreens; i++)
        if (!strcmp(st->scr_id[i], pi)) break;
    if (i == st->nscreens) {
        if (st->nscreens == 8) {                     /* (keep the newest 8 Zero Ichi cameras) */
            memmove(st->scr_id[0], st->scr_id[1], sizeof st->scr_id[0] * 7);
            memmove(st->scr_name[0], st->scr_name[1], sizeof st->scr_name[0] * 7);
            st->nscreens--;
        }
        i = st->nscreens++;
        snprintf(st->scr_id[i], sizeof st->scr_id[i], "%s", pi);
    }
    snprintf(st->scr_name[i], sizeof st->scr_name[i], "%s", name);
}

double state_space(const State *st, const char *key) {
    for (int i = 0; i < st->nspace; i++)
        if (!strcmp(st->space_key[i], key)) return st->space_val[i];
    return -1;
}

void state_set_space(State *st, const char *key, double v) {
    for (int i = 0; i < st->nspace; i++)
        if (!strcmp(st->space_key[i], key)) {
            st->space_val[i] = v;
            return;
        }
    if (st->nspace == NSPACE) {                  /* keep the newest 40 */
        memmove(st->space_key[0], st->space_key[1], sizeof st->space_key[0] * (NSPACE - 1));
        memmove(&st->space_val[0], &st->space_val[1], sizeof st->space_val[0] * (NSPACE - 1));
        st->nspace--;
    }
    snprintf(st->space_key[st->nspace], sizeof st->space_key[0], "%s", key);
    st->space_val[st->nspace++] = v;
}

/* ---------------------------------------------------------------- presets */
int preset_write(const State *st, const char *screen, const char *camera, const char *path, int number) {
    char when[32] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    char text[1024];
    snprintf(text, sizeof text,
             "# Zero Ichi preset %d (saved %s)\n"
             "# Load it on the camera: MENU > Presets, choose %d, KEY1.\n"
             "# You can edit this file on a computer, or copy it to another Zero Ichi.\n"
             "look = %s\nmode = %s\nev = %+.1f\nrotation = %d\nimage_ratio = %s\nmirror = %s\n"
             "flip_screen = %s\nclip_size = %s\ngif_frames = %d\nvideo_max = %d\n"
             "screen = %s\nscreen_saver = %s\nkey3_hold = %s\n"
             "# (the Zero Ichi it was saved on: its screen is only used there - another\n"
             "#  Zero Ichi keeps its own screen)\n"
             "camera = %s\n",
             number, when, number, LOOK_NAMES[st->filter], st->mode, st->ev, st->cam_rotate, ratio_label(st->size),
             st->mirror ? "on" : "off", st->screen_flip ? "on" : "off", st->clip_size,
             st->gif_frames, st->video_max, screen, st->screensaver, st->key3,
             camera && camera[0] ? camera : "-");
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    char *sl = strrchr(dir, '/');
    if (sl) {
        *sl = 0;
        mkdir_p(dir);
    }
    int r = write_file_atomic(path, text, strlen(text));
    sync_all();
    return r;
}

static int pick(const char *v, const char *const *list, int n, char *out, size_t cap) {
    for (int i = 0; i < n; i++)
        if (!strcasecmp(v, list[i])) {
            snprintf(out, cap, "%s", list[i]);
            return 1;
        }
    return 0;
}

int preset_read(State *st, const char *path, char *screen, size_t cap, char *camera, size_t ccap) {
    char *t = read_file(path, NULL);
    if (!t) return -1;
    if (!strncmp(strip(t), "{\\rtf", 5)) strip_rtf(t);
    static const char *const MODES[] = {"PHOTO", "GIF", "VIDEO"};
    static const char *const KEY3[] = {"Power off", "Standby"};
    int got = 0;
    if (screen && cap) screen[0] = 0;
    if (camera && ccap) camera[0] = 0;
    for (char *line = strtok(t, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = strip(line), *val = strip(eq + 1);
        lower(key);
        for (char *k = key; *k; k++) if (*k == '-' || *k == ' ') *k = '_';
        if (!*val) continue;
        got++;
        if (!strcmp(key, "look") || !strcmp(key, "filter")) {
            int id = look_by_name(val);
            for (int i = 0; id < 0 && i < NLOOKS; i++)
                if (!strcasecmp(val, LOOK_NAMES[i])) id = i;
            if (id >= 0) st->filter = id;
        } else if (!strcmp(key, "mode")) pick(val, MODES, 3, st->mode, sizeof st->mode);
        else if (!strcmp(key, "ev")) {
            double e = strtod(val, NULL);
            e = floor(e * 2 + 0.5) / 2;
            st->ev = e < -2 ? -2 : e > 2 ? 2 : e;
        } else if (!strcmp(key, "size") || !strcmp(key, "image_ratio") || !strcmp(key, "ratio"))
            ratio_pick(val, st->size, sizeof st->size);
        else if (!strcmp(key, "mirror")) st->mirror = yes(val);
        else if (!strcmp(key, "gif_frames")) st->gif_frames = atoi(val);
        else if (!strcmp(key, "clip_size")) {
            char c[16];
            snprintf(c, sizeof c, "%s", val);
            lower(c);
            clip_canon(c, sizeof c);
            pick(c, CLIP_NAMES, 2, st->clip_size, sizeof st->clip_size);
        }
        else if (!strcmp(key, "video_max")) st->video_max = atoi(val);
        else if (!strcmp(key, "rotation") || !strcmp(key, "cam_rotate")) st->cam_rotate = atoi(val);
        else if (!strcmp(key, "flip_screen") || !strcmp(key, "screen_flip")) st->screen_flip = yes(val);
        else if (!strcmp(key, "screen_saver") || !strcmp(key, "screensaver")) pick(val, SAVERS, 5, st->screensaver, sizeof st->screensaver);
        else if (!strcmp(key, "key3_hold") || !strcmp(key, "key3")) pick(val, KEY3, 2, st->key3, sizeof st->key3);
        else if (!strcmp(key, "screen")) {
            if (screen && (!strcmp(val, "320x240") || !strcmp(val, "240x240"))) snprintf(screen, cap, "%s", val);
        }
        else if (!strcmp(key, "camera")) {
            if (camera && ccap) snprintf(camera, ccap, "%s", val);
            got--;                                 /* (not a setting) */
        }
        else got--;
    }
    free(t);
    state_validate(st);
    return got > 0 ? 0 : -1;
}
