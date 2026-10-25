/* Plan 6 Task 7 (spec §8.1–§8.3, §8.6): the firmware rows in Settings, their confirms and the boot-note toasts.
   Every Host call runs on the Sessions worker (P6-16: ui_sessions_fw_check, ui_sessions_fw_start); the worker hands
   its result to the LVGL task with ui_async, so no timer of its own. */
#include "ui_settings_fw.h"
#include "ui.h"
#include "ui_comp.h"
#include "ui_fwupdate.h"
#include "ui_sessions.h"
#include "ui_settings.h"
#include "companion/cc_link.h"
#include "ota/fw_update.h"
#include "ota/fw_rules.h"
#include "esp_timer.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static volatile bool s_checking;        /* a check job is queued or running */
static volatile bool s_starting;        /* an Install job is queued or running */
static bool s_confirm_after;            /* the row was activated: open the confirm when the check finds an image */
static lv_obj_t *s_row;                 /* About's Update firmware row (NULL when not shown) */
static char s_title[64], s_body[400];   /* the open confirm's text */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

#ifdef UI_PREVIEW
bool pv_fw_check_hold(void);   /* the preview: leave the check pending (PREVIEW_FW_CHECK=checking) */
#endif

/* ------------------------------------------------------------------ Host */
static void fw_path_prompt_cb(const char *v, void *ud)
{
    (void)ud;
    if (!v) return;
    if (!cc_link_set_fw_path(v)) { ui_toast(tr(STR_SET_CLAUDE_PATH_BAD)); return; }
    ui_settings_refill();
}

static void fw_path_cb(lv_event_t *e)
{
    (void)e;
    char cur[64];
    ui_prompt(tr(STR_SET_FW_PATH), tr(STR_SET_FW_PATH_PH), cc_settings_fw_path(cur, sizeof cur), false, fw_path_prompt_cb,
              NULL);
}

void ui_settings_fw_host_row(lv_obj_t *p)
{
    char v[64];
    ui_srow(p, UI_ROW_VALUE, tr(STR_SET_FW_PATH), cc_settings_fw_path(v, sizeof v), fw_path_cb, NULL);
}

/* ----------------------------------------------------------------- About */
/* The first sentence of an error's copy, for the row (spec §8.6). */
static void first_sentence(const char *s, char *out, size_t cap)
{
    const char *end = strstr(s, ". ");
    size_t n = end ? (size_t)(end - s + 1) : strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

static bool fresh(const fwu_check_t *c) { return now_ms() - c->at_ms < FW_CHECK_TTL_MS; }
static bool available(const fwu_check_t *c) { return c->out == FW_OUT_OK && c->offer != FW_OFFER_UP_TO_DATE; }

/* The row's value and tone: Checking… / Check / Up to date (muted), "<v> available" (fg), an error (red). */
static void row_value(char *out, size_t cap, ui_tone_t *tone)
{
    fwu_check_t c;
    *tone = UI_TONE_MUTED;
    if (s_checking) { snprintf(out, cap, "%s", tr(STR_SET_FW_CHECKING)); return; }
    if (!fwu_last_check(&c)) { snprintf(out, cap, "%s", tr(STR_SET_FW_CHECK)); return; }
    if (c.out != FW_OUT_OK) { first_sentence(tr(fw_out_text(c.out)), out, cap); *tone = UI_TONE_RED; return; }
    if (c.offer == FW_OFFER_UP_TO_DATE) { snprintf(out, cap, "%s", tr(STR_SET_FW_UP_TO_DATE)); return; }
    snprintf(out, cap, tr(c.offer == FW_OFFER_OLDER ? STR_SET_FW_AVAILABLE_OLDER : STR_SET_FW_AVAILABLE), c.desc.version);
    *tone = UI_TONE_FG;
}

static void row_update(void)
{
    if (!s_row) return;
    lv_obj_t *v = ui_srow_value(s_row);
    if (!v) return;
    char b[96];
    ui_tone_t tone;
    row_value(b, sizeof b, &tone);
    if (strcmp(lv_label_get_text(v), b)) lv_label_set_text(v, b);
    ui_text_tone(v, tone);
}

/* The confirm's primary button takes the first key (Enter installs, Esc cancels: spec §8.3). */
static lv_obj_t *find_text(lv_obj_t *o, const char *t)
{
    if (lv_obj_check_type(o, &lv_label_class) && !strcmp(lv_label_get_text(o), t)) return o;
    for (uint32_t i = 0; i < lv_obj_get_child_count(o); i++) {
        lv_obj_t *r = find_text(lv_obj_get_child(o, i), t);
        if (r) return r;
    }
    return NULL;
}

static void focus_action(const char *action)
{
    lv_obj_t *top = lv_layer_top();
    uint32_t n = lv_obj_get_child_count(top);
    if (!n) return;
    lv_obj_t *l = find_text(lv_obj_get_child(top, n - 1), action);
    while (l && !lv_obj_has_flag(l, LV_OBJ_FLAG_CLICKABLE)) l = lv_obj_get_parent(l);
    if (l) lv_obj_add_flag(l, UI_KEYBOARD_FIRST);
}

static bool request_check(bool confirm_after)
{
    if (s_checking) return false;
    s_checking = true;
    s_confirm_after = confirm_after;
    row_update();
    if (!ui_sessions_fw_check()) { s_checking = false; s_confirm_after = false; row_update(); return false; }
    return true;
}

static void install_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes || s_starting) return;
    s_starting = true;
    if (!ui_sessions_fw_start()) { s_starting = false; ui_toast(tr(STR_SET_BUSY)); }
}

static void open_confirm(const fwu_check_t *c)
{
    snprintf(s_title, sizeof s_title, tr(STR_FW_CONFIRM_Q), c->desc.version);
    size_t n = 0;
    s_body[0] = 0;
    if (c->offer == FW_OFFER_OLDER) n = (size_t)snprintf(s_body, sizeof s_body, "%s\n", tr(STR_FW_CONFIRM_OLDER));
    if (n < sizeof s_body) snprintf(s_body + n, sizeof s_body - n, tr(STR_FW_CONFIRM_BODY), fwu_running_version());
    ui_confirm_ex(s_title, s_body, tr(STR_FW_INSTALL), false, install_yes, NULL);
    focus_action(tr(STR_FW_INSTALL));
}

static void fw_row_cb(lv_event_t *e)
{
    (void)e;
    if (s_checking) return;
    fwu_check_t c;
    bool have = fwu_last_check(&c);
    if (have && fresh(&c) && available(&c)) { open_confirm(&c); return; }
    /* none, stale, failed: check, then the confirm if an image is there; Up to date: just check again */
    bool up_to_date = have && fresh(&c) && c.out == FW_OUT_OK;
    if (!request_check(!up_to_date)) ui_toast(tr(STR_SET_BUSY));
}

/* On the LVGL task, after the worker's check */
static void check_done_cb(void *arg)
{
    (void)arg;
    row_update();
    bool confirm = s_confirm_after;
    s_confirm_after = false;
    fwu_check_t c;
    if (confirm && s_row && ui_nav_is_top(ui_settings_create) && fwu_last_check(&c) && available(&c)) open_confirm(&c);
}

void ui_fw_job_check(void)
{
#ifdef UI_PREVIEW
    if (pv_fw_check_hold()) return;   /* stays "Checking…" */
#endif
    fwu_check(NULL);
    s_checking = false;
    ui_async(check_done_cb, NULL);
}

/* The Install job's end on the LVGL task: arg is -1 (the update runs: the progress screen) or a str_id_t to toast. */
static void start_done_cb(void *arg)
{
    intptr_t v = (intptr_t)arg;
    s_starting = false;
    row_update();
    if (v < 0) ui_fwupdate_show(NULL);
    else ui_toast_ms(tr((str_id_t)v), 6000);
}

void ui_fw_job_start(void)
{
    fw_out_t o = fwu_start();
    if (o == FW_OUT_CHANGED) {   /* the check was stale or gone: check again, then start once more */
        fwu_check_t c;
        fwu_check(&c);
        if (c.out != FW_OUT_OK) o = c.out;
        else if (c.offer == FW_OFFER_UP_TO_DATE) { ui_async(start_done_cb, (void *)(intptr_t)STR_SET_FW_UP_TO_DATE); return; }
        else o = fwu_start();
    }
    if (o == FW_OUT_OK) { ui_async(start_done_cb, (void *)(intptr_t)-1); return; }
    str_id_t id = fw_out_text(o);
    ui_async(start_done_cb, (void *)(intptr_t)(id == STR_COUNT ? STR_FW_ERR_WRITE : id));
}

/* Previous version › Go back */
static void go_back_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    if (fwu_go_back() != ESP_OK) ui_toast_ms(tr(STR_FW_ERR_WRITE), 6000);   /* returns only on a failure */
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    fw_desc_t d;
    if (!fwu_previous(&d)) { ui_settings_refill(); return; }
    snprintf(s_title, sizeof s_title, tr(STR_FW_GO_BACK_Q), d.version);
    ui_confirm_ex(s_title, tr(STR_FW_GO_BACK_BODY), tr(STR_SET_FW_GO_BACK), false, go_back_yes, NULL);
    focus_action(tr(STR_SET_FW_GO_BACK));
}

static void row_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_row) s_row = NULL;
}

void ui_settings_fw_about(lv_obj_t *p)
{
    char b[96];
    ui_tone_t tone;
    row_value(b, sizeof b, &tone);
    s_row = ui_srow(p, UI_ROW_VALUE, tr(STR_SET_FW_UPDATE), b, fw_row_cb, NULL);
    lv_obj_add_event_cb(s_row, row_deleted, LV_EVENT_DELETE, NULL);
    ui_text_tone(ui_srow_value(s_row), tone);
    fw_desc_t d;
    if (fwu_previous(&d)) {
        snprintf(b, sizeof b, "%s \xc2\xb7 %s", d.version, tr(STR_SET_FW_GO_BACK));
        ui_srow(p, UI_ROW_VALUE, tr(STR_SET_FW_PREVIOUS), b, prev_cb, NULL);
    }
    ui_footnote(p, tr(STR_SET_FW_NOTE));
    /* the background check when About is shown (P6-16, P6-19: About and the console only) */
    fwu_check_t c;
    if (!s_checking && !fwu_busy() && !(fwu_last_check(&c) && fresh(&c))) request_check(false);
}

/* ------------------------------------------------------------- boot notes */
void ui_fw_note_tick(void)
{
    fwu_note_t n;
    if (!fwu_take_boot_note(&n)) return;
    char b[200];
    switch (n.kind) {
    case FWU_NOTE_UPDATED:      snprintf(b, sizeof b, tr(STR_FW_UPDATED), n.ver_a); break;
    case FWU_NOTE_ROLLED_BACK:  snprintf(b, sizeof b, tr(STR_FW_ROLLED_BACK), n.ver_a, n.ver_b); break;
    case FWU_NOTE_BOOTLOOP:     snprintf(b, sizeof b, tr(STR_FW_BOOTLOOP), n.ver_a); break;
    case FWU_NOTE_ROLLING_BACK: snprintf(b, sizeof b, tr(STR_FW_ROLLING_BACK), n.ver_a); break;
    default: return;
    }
    ui_toast_ms(b, 6000);
}
