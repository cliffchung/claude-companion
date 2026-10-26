/* The firmware update's progress screen (Plan 6 Task 7, spec §8.4): a full-screen modal on lv_layer_top, built once
   per update. One 300 ms timer (B1) refreshes it from fwu_status and goes with it. The modal owns every key: Esc
   cancels (LV_EVENT_CANCEL), everything else is eaten (Cancel is touch-only, so Enter never presses it). No amber:
   the bar is fg on line, an error is a red toast. */
#include "ui_fwupdate.h"
#include "ui.h"
#include "ui_comp.h"
#include "ui_settings.h"
#include "ota/fw_update.h"
#include "ota/fw_rules.h"
#include <stdio.h>
#include <string.h>

#define FW_TICK_MS 300
#define BAR_W      600
#define BAR_H      12

static lv_obj_t *s_ov, *s_phase, *s_bar, *s_bytes, *s_hint, *s_cancel;
static lv_timer_t *s_timer;
static fwu_phase_t s_last_phase = (fwu_phase_t)-1;

bool ui_fwupdate_shown(void) { return s_ov != NULL; }
lv_timer_t *ui_fwupdate_timer(void) { return s_timer; }

static str_id_t phase_text(fwu_phase_t p)
{
    switch (p) {
    case FWU_CHECKING:   return STR_FW_PHASE_CHECK;
    case FWU_WRITING:    return STR_FW_PHASE_WRITE;
    case FWU_READBACK:   return STR_FW_PHASE_VERIFY;
    case FWU_RESTARTING: return STR_FW_PHASE_RESTART;
    default:             return STR_FW_PHASE_DOWNLOAD;
    }
}

/* A label write redraws: only on a change. */
static void set_text(lv_obj_t *l, const char *t) { if (strcmp(lv_label_get_text(l), t)) lv_label_set_text(l, t); }

static void close_screen(void)
{
    lv_obj_t *ov = s_ov;
    s_ov = NULL;
    if (ov) lv_obj_delete(ov);   /* the timer goes with it (ui_timer_bind) */
}

static void refresh(void)
{
    fwu_status_t st;
    fwu_status(&st);
    if (st.phase == FWU_IDLE) {
        /* the update ended: close, then the outcome (P6-17) */
        fwu_note_t n;
        bool got = fwu_take_result(&n);   /* fix M3: a boot note waiting for Sessions stays */
        close_screen();
        if (got && n.kind == FWU_NOTE_RESULT && fw_out_text(n.out) != STR_COUNT) ui_toast_ms(tr(fw_out_text(n.out)), 6000);
        ui_settings_refill();   /* About's row shows the new state (nothing when Settings isn't on top) */
        return;
    }
    int pct = st.phase == FWU_RESTARTING ? 100 : fw_pct(st.done, st.total);
    set_text(s_phase, tr(phase_text(st.phase)));
    if (lv_bar_get_value(s_bar) != pct) lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
    char mb[40], b[64];
    fw_mb_text(st.phase == FWU_RESTARTING ? st.total : st.done, st.total, mb, sizeof mb);
    snprintf(b, sizeof b, tr(STR_FW_BYTES), mb, pct);
    set_text(s_bytes, b);
    if (st.phase == FWU_RESTARTING && st.phase != s_last_phase) {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_cancel, LV_OBJ_FLAG_HIDDEN);
    }
    s_last_phase = st.phase;
}

static void tick_cb(lv_timer_t *t) { (void)t; if (s_ov) refresh(); }

static void cancel_cb(lv_event_t *e) { (void)e; if (!fwu_cancel()) ui_toast(tr(STR_FW_TOO_LATE)); }   /* core M6 */

static void deleted_cb(lv_event_t *e)
{
    (void)e;
    s_ov = NULL;
    s_timer = NULL;
}

void ui_fwupdate_show(void *arg)
{
    (void)arg;
    if (s_ov) return;
    s_last_phase = (fwu_phase_t)-1;
    s_ov = ui_modal_layer();
    lv_obj_add_event_cb(s_ov, deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(s_ov, cancel_cb, LV_EVENT_CANCEL, NULL);   /* Esc */
    lv_obj_t *page = lv_obj_create(s_ov);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(page, lv_color_hex(UI_TK_BG), 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(page, UI_TK_GAP_L, 0);
    lv_obj_set_style_pad_hor(page, UI_TK_MARGIN, 0);

    ui_title(page, tr(STR_FW_TITLE));
    s_phase = ui_text(page, tr(STR_FW_PHASE_DOWNLOAD), UI_TONE_FG, false);
    s_bar = lv_bar_create(page);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, BAR_W, BAR_H);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_radius(s_bar, BAR_H / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_TK_LINE), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, BAR_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_TK_FG), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    s_bytes = ui_text(page, "", UI_TONE_FG, true);
    s_hint = ui_text(page, tr(STR_FW_HINT), UI_TONE_MUTED, true);
    lv_obj_set_width(s_hint, LV_PCT(100));
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_MODE_WRAP);
    s_cancel = ui_btn(page, UI_BTN_SECONDARY, tr(STR_CANCEL), cancel_cb, NULL);
    lv_obj_add_flag(s_cancel, UI_KEYBOARD_SKIP);   /* touch only: Enter must not cancel (every other key is eaten) */

    s_timer = lv_timer_create(tick_cb, FW_TICK_MS, NULL);
    ui_timer_bind(s_timer, s_ov);
    refresh();
}
