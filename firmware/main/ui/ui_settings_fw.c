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
    if (!cc_link_set_fw_path(v)) { ui_toast(tr(STR_SET_FW_PATH_BAD)); return; }
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

/* The row's value and tone: Checking… / Check / Up to date (muted), "<v> available" (fg), an error or Rollback off
   (red). */
static void row_value(char *out, size_t cap, ui_tone_t *tone)
{
    fwu_check_t c;
    *tone = UI_TONE_MUTED;
    if (!fwu_layout_ok()) { snprintf(out, cap, "%s", tr(STR_SET_FW_LAYOUT_OLD)); *tone = UI_TONE_RED; return; }   /* N8 */
    if (!fwu_rollback_ok()) { snprintf(out, cap, "%s", tr(STR_SET_FW_ROLLBACK_OFF)); *tone = UI_TONE_RED; return; }
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

static bool request_check(bool confirm_after)
{
    if (s_checking) return false;
    s_checking = true;
    s_confirm_after = confirm_after;
    row_update();
    if (!ui_sessions_fw_check()) { s_checking = false; s_confirm_after = false; row_update(); return false; }
    return true;
}

/* The Install confirm uses the shared rule (main's stop-hotkey): opened by a key, Cancel has the focus and the
   arrows never reach Install; Tab or a tap does (fix I1). s_shown_sha is the build it names (fix I3). */
static bool s_confirm_open;
static char s_shown_sha[65];

static void install_yes(bool yes, void *ud)
{
    (void)ud;
    s_confirm_open = false;
    if (!yes || s_starting) return;
    s_starting = true;
    if (!ui_sessions_fw_start()) { s_starting = false; ui_toast(tr(STR_SET_BUSY)); }
}

static void open_confirm(const fwu_check_t *c)
{
    if (s_confirm_open) return;   /* fix M2: never two */
    s_confirm_open = true;
    snprintf(s_shown_sha, sizeof s_shown_sha, "%s", c->sha);
    snprintf(s_title, sizeof s_title, tr(STR_FW_CONFIRM_Q), c->desc.version);
    size_t n = 0;
    s_body[0] = 0;
    if (c->offer == FW_OFFER_OLDER) n = (size_t)snprintf(s_body, sizeof s_body, "%s\n", tr(STR_FW_CONFIRM_OLDER));
    if (n < sizeof s_body) snprintf(s_body + n, sizeof s_body - n, tr(STR_FW_CONFIRM_BODY), fwu_running_version());
    ui_confirm_ex(s_title, s_body, tr(STR_FW_INSTALL), false, install_yes, NULL);
}

static void fw_row_cb(lv_event_t *e)
{
    (void)e;
    if (!fwu_layout_ok()) { ui_toast_ms(tr(STR_FW_ERR_LAYOUT), 6000); return; }
    if (!fwu_rollback_ok()) { ui_toast_ms(tr(STR_FW_ERR_NO_ROLLBACK), 6000); return; }
    if (s_checking || s_confirm_open) return;
    fwu_check_t c;
    bool have = fwu_last_check(&c);
    if (have && fresh(&c) && available(&c)) { open_confirm(&c); return; }
    /* none, stale, failed: check, then the confirm if an image is there; Up to date: just check again */
    bool up_to_date = have && fresh(&c) && c.out == FW_OUT_OK;
    if (!request_check(!up_to_date)) ui_toast(tr(STR_SET_BUSY));
}

/* On the LVGL task, after the worker's check. s_checking clears here, not on the worker, so a tap can't open a
   confirm between the two (fix M2); nothing opens over another dialog. */
static void check_done_cb(void *arg)
{
    (void)arg;
    s_checking = false;
    row_update();
    bool confirm = s_confirm_after;
    s_confirm_after = false;
    fwu_check_t c;
    if (confirm && s_row && ui_nav_is_top(ui_settings_create) && !ui_keyboard_owns_input() && fwu_last_check(&c) &&
        available(&c))
        open_confirm(&c);
}

void ui_fw_job_check(void)
{
#ifdef UI_PREVIEW
    if (pv_fw_check_hold()) return;   /* stays "Checking…" */
#endif
    fwu_check(NULL);
    ui_async(check_done_cb, NULL);
}

/* The Install job's end on the LVGL task: START_RUNS (the progress screen), START_RECONFIRM (the staged build changed
   since the confirm: a new confirm, fix I3) or a str_id_t to toast. */
#define START_RUNS      (-1)
#define START_RECONFIRM (-2)
static void start_done_cb(void *arg)
{
    intptr_t v = (intptr_t)arg;
    s_starting = false;
    row_update();
    fwu_check_t c;
    if (v == START_RUNS) ui_fwupdate_show(NULL);
    else if (v == START_RECONFIRM) { if (fwu_last_check(&c) && available(&c)) open_confirm(&c); }
    else ui_toast_ms(tr((str_id_t)v), 6000);
}

void ui_fw_job_start(void)
{
    char shown[65];
    snprintf(shown, sizeof shown, "%s", s_shown_sha);
    fw_out_t o = fwu_start_expect(shown);
    if (o == FW_OUT_CHANGED) {   /* the check was stale, gone or another build: check again */
        fwu_check_t c;
        fwu_check(&c);
        if (c.out != FW_OUT_OK) o = c.out;
        else if (c.offer == FW_OFFER_UP_TO_DATE) { ui_async(start_done_cb, (void *)(intptr_t)STR_SET_FW_UP_TO_DATE); return; }
        else if (!fw_confirm_matches(shown, c.sha)) { ui_async(start_done_cb, (void *)(intptr_t)START_RECONFIRM); return; }
        else o = fwu_start_expect(shown);   /* the same build, only stale: the confirm still holds */
    }
    if (o == FW_OUT_OK) { ui_async(start_done_cb, (void *)(intptr_t)START_RUNS); return; }
    str_id_t id = fw_out_text(o);
    ui_async(start_done_cb, (void *)(intptr_t)(id == STR_COUNT ? STR_FW_ERR_WRITE : id));
}

/* Previous version › Go back: the verification and the restart run on the worker, with the same gates as Install
   (busy, a session, voice, the battery: fix M1). The job returns only on a refusal or a failure. */
static void go_back_done_cb(void *arg) { ui_toast_ms(tr((str_id_t)(intptr_t)arg), 6000); }

void ui_fw_job_go_back(void)
{
    fw_out_t o = fwu_go_back();
    str_id_t id = fw_out_text(o);
    ui_async(go_back_done_cb, (void *)(intptr_t)(id == STR_COUNT ? STR_FW_ERR_WRITE : id));
}

static void go_back_yes(bool yes, void *ud)
{
    (void)ud;
    if (!yes) return;
    if (!ui_sessions_fw_go_back()) ui_toast(tr(STR_SET_BUSY));
}

static void prev_cb(lv_event_t *e)
{
    (void)e;
    fw_desc_t d;
    if (!fwu_previous(&d)) { ui_settings_refill(); return; }
    snprintf(s_title, sizeof s_title, tr(STR_FW_GO_BACK_Q), d.version);
    ui_confirm_ex(s_title, tr(STR_FW_GO_BACK_BODY), tr(STR_SET_FW_GO_BACK), false, go_back_yes, NULL);
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
    if (fwu_layout_ok() && fwu_rollback_ok() && !s_checking && !fwu_busy() && !(fwu_last_check(&c) && fresh(&c))) request_check(false);
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
    case FWU_NOTE_ROLLBACK_FAILED: snprintf(b, sizeof b, "%s", tr(STR_FW_ROLLBACK_FAILED)); break;
    case FWU_NOTE_BOOTLOOP_BOTH: snprintf(b, sizeof b, "%s", tr(STR_FW_BOOTLOOP_BOTH)); break;
    default: return;
    }
    ui_toast_ms(b, 6000);
}
