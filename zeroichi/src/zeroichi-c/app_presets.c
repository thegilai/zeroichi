/* Zero Ichi - MENU > Presets: all your settings (the look, mode and EV and
 * every MENU setting - see preset_write) saved as small text files on the
 * card - zeroichi/presets/preset-1.txt ... - and loaded with one key. They are
 * kept nowhere else, so they travel with the card. */
#include <stdio.h>
#include <string.h>
#include "app.h"
#include "gpio.h"
#include "screensaver.h"
#include "util.h"

static char summary[NPRESETS][64];
static int have[NPRESETS], nsaved;
static int confirm_slot = -1;
static double confirm_until;
static char label[24];

static void preset_path(int i, char *out, size_t cap) {
    snprintf(out, cap, "%s/presets/preset-%d.txt", A.data_dir, i + 1);
}

static void describe(const State *t, char *out, size_t cap) {
    snprintf(out, cap, "%s • %s", LOOK_NAMES[t->filter], t->mode);
}

/* what each preset holds (read when the menu opens, and after saving) */
void presets_refresh(void) {
    nsaved = 0;
    for (int i = 0; i < NPRESETS; i++) {
        char p[700];
        preset_path(i, p, sizeof p);
        State t = A.st;
        have[i] = file_exists(p) && preset_read(&t, p, NULL, 0, NULL, 0) == 0;
        if (have[i]) {
            describe(&t, summary[i], sizeof summary[i]);
            nsaved++;
        } else {
            snprintf(summary[i], sizeof summary[i], "(empty)");
        }
    }
    snprintf(label, sizeof label, nsaved ? "%d saved" : "None", nsaved);
    confirm_slot = -1;
}

const char *presets_label(void) { return label[0] ? label : "None"; }

/* (v52) in Zero Ichi's design - the menu's card (app_view.c: ui_*) */
void presets_draw(void) {
    int confirm = confirm_slot == A.preset_idx && mono() < confirm_until;
    char key[512];
    int k = snprintf(key, sizeof key, "presets %d %d %d", A.preset_idx, confirm, A.W);
    for (int i = 0; i < NPRESETS; i++) k += snprintf(key + k, sizeof key - k > 0 ? sizeof key - k : 0, "|%s", summary[i]);
    if (ui_card_reuse(key)) return;
    ui_card("presets", 0);
    const Font *f = brand_font(14, 0);
    int y = 52;
    for (int i = 0; i < NPRESETS; i++, y += 24) {
        char row[96], num[8];
        snprintf(num, sizeof num, "%d", i + 1);
        snprintf(row, sizeof row, "%s", summary[i]);
        while (text_width(f, row) > A.W - 66 && strlen(row) > 4) {   /* too long: shortened... */
            size_t l = strlen(row);
            row[l - 4] = 0;
            strcat(row, "\xe2\x80\xa6");
        }
        int chosen = i == A.preset_idx;
        if (chosen) {
            cv_rect(&A.scr, 10, y - 10, A.W - 11, y + 10, ZI_INK);
            ui_circle(&A.scr, 17, y + 0.5f, 3.2f, ZI_RED);
        }
        cv_text_f(&A.scr, f, 26, y + 0.5, num, chosen ? ZI_YEL : ZI_BLU, "lm");
        cv_text_f(&A.scr, f, 44, y + 0.5, row, chosen ? ZI_CREAM : have[i] ? ZI_INK : ZI_GREY, "lm");
    }
    cv_text_f(&A.scr, brand_font(12, 0), 14, A.H - 36, confirm ? "KEY2 again: replace it" : "KEY1: load    KEY2: save",
              confirm ? ZI_RED : ZI_INK, "lm");
    ui_hint("PRESS: back", 0);
    ui_card_keep(key);
}

static void load(int i) {
    char p[700];
    preset_path(i, p, sizeof p);
    State t = A.st, old = A.st;
    char scr[16], cam[24];
    if (!have[i] || preset_read(&t, p, scr, sizeof scr, cam, sizeof cam)) {
        show_toast("This preset is empty\n(KEY2 saves one)", 2);
        return;
    }
    /* The screen is part of the Zero Ichi itself: a preset saved on another
     * Zero Ichi (or before v43) keeps this one's own screen - the rest loads. */
    const char *me = this_pi();
    if (scr[0] && me[0] && strcmp(cam, me) && state_screen_of(&A.st, me)) {
        char cur[16];
        snprintf(cur, sizeof cur, "%dx%d", A.W, A.H);
        if (strcmp(scr, cur))
            logi("Preset %d: its screen (%s) is another Zero Ichi's - this one keeps %s", i + 1, scr, cur);
        scr[0] = 0;
    }
    A.st = t;
    A.presets_open = 0;
    A.screen = SCR_CAMERA;
    state_loaded(&old, scr);
    char d[64], msg[96];
    describe(&A.st, d, sizeof d);
    logi("Preset %d loaded (%s)", i + 1, d);
    snprintf(msg, sizeof msg, "Preset %d loaded", i + 1);
    show_toast(msg, 2);
}

static void save(int i) {
    if (have[i] && !(confirm_slot == i && mono() < confirm_until)) {   /* (replace: press again) */
        confirm_slot = i;
        confirm_until = mono() + 3;
        return;
    }
    char p[700], d[64], msg[96], scr[16];
    preset_path(i, p, sizeof p);
    describe(&A.st, d, sizeof d);
    snprintf(scr, sizeof scr, "%dx%d", A.W, A.H);
    if (preset_write(&A.st, scr, this_pi(), p, i + 1)) {
        loge("Could not save preset %d to %s", i + 1, p);
        show_toast("Could not save\n(is the SD card full?)", 2.5);
        return;
    }
    logi("Preset %d saved (%s)", i + 1, d);
    presets_refresh();
    snprintf(msg, sizeof msg, "Saved as preset %d\n(on the SD card)", i + 1);
    show_toast(msg, 2);
}

void presets_key(int b) {
    if (A.preset_idx < 0 || A.preset_idx >= NPRESETS) A.preset_idx = 0;
    if (b == BTN_UP || b == BTN_DOWN) {
        A.preset_idx = (A.preset_idx + (b == BTN_DOWN ? 1 : -1) + NPRESETS) % NPRESETS;
        confirm_slot = -1;
    } else if (b == BTN_KEY1) {
        load(A.preset_idx);
    } else if (b == BTN_KEY2) {
        save(A.preset_idx);
    } else if (b == BTN_PRESS || b == BTN_LEFT) {
        A.presets_open = 0;
    }
}
