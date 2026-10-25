#include "ui.h"
#include "ui_comp.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "input/keyboard.h"
#include "sys/power.h"
#include "src/misc/lv_event_private.h" /* LVGL 9.6 has no public event-filter accessor. */

static const char *TAG = "ui";

static int s_modal_count;   /* a swipe must not navigate while a dialog is up */
static void keyboard_focus_set(lv_obj_t *obj);
static bool s_keyboard_terminal_ui;

/* ------------------------------------------------------------------ nav */
typedef struct { ui_screen_create_fn create; void *arg; } nav_entry_t;
#define NAV_MAX 12
static nav_entry_t s_stack[NAV_MAX];
static int s_depth = 0;
static lv_obj_t *s_current = NULL;

static lv_obj_t *s_keyboard_scope;
static lv_obj_t *s_blank;   /* P4-2: active while the old screen is deleted and the next one built */

/* Spec §16 B2: instant, and never two screens alive at once. Load the blank, delete the old
 * screen, then create and load the new one. No fade, no slide. */
static void nav_show_top(void)
{
    if (s_depth <= 0) return;
    keyboard_focus_set(NULL);
    s_keyboard_terminal_ui = false;
    lv_obj_t *old = lv_screen_active();
    if (old && old != s_blank) {
        lv_screen_load(s_blank);
        lv_obj_delete(old);
    }
    s_current = NULL;
    s_keyboard_scope = NULL;   /* the next screen may reuse the deleted one's address */
    nav_entry_t *e = &s_stack[s_depth - 1];
    lv_obj_t *scr = e->create(e->arg);
    if (!scr) {
        /* never leave the blank up: fall back one level (home stays on the blank if it can't be built) */
        ESP_LOGE(TAG, "screen create failed");
        if (s_depth > 1) { s_depth--; nav_show_top(); }
        return;
    }
    lv_screen_load(scr);
    s_current = scr;
}

/* A screen's back guard (the host form's "Discard changes?"): asked first by every way off its screen (back, home,
   push, replace) while that screen is current; true means it is asking, and the move waits as the pending one
   until the screen calls ui_nav_proceed (or ui_nav_cancel_pending). Forgotten when the screen is deleted. */
typedef enum { NAV_BACK, NAV_HOME, NAV_PUSH, NAV_REPLACE } nav_op_t;
static lv_obj_t *s_guard_scr;
static ui_back_guard_t s_guard;
static struct { bool set; nav_op_t op; ui_screen_create_fn create; void *arg; } s_pending;
static void guard_scr_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_guard_scr) { s_guard_scr = NULL; s_guard = NULL; s_pending.set = false; }
}
void ui_nav_set_back_guard(lv_obj_t *scr, ui_back_guard_t guard)
{
    if (s_guard_scr) lv_obj_remove_event_cb(s_guard_scr, guard_scr_deleted);
    s_guard_scr = guard ? scr : NULL;
    s_guard = scr ? guard : NULL;
    s_pending.set = false;
    if (s_guard_scr) lv_obj_add_event_cb(s_guard_scr, guard_scr_deleted, LV_EVENT_DELETE, NULL);
}
static bool guarded(nav_op_t op, ui_screen_create_fn create, void *arg)
{
    if (!s_guard || !s_guard_scr || s_guard_scr != lv_screen_active()) return false;
    bool asked = s_pending.set;   /* a question is up: the first move keeps waiting; this one is ignored */
    if (!asked) { s_pending.set = true; s_pending.op = op; s_pending.create = create; s_pending.arg = arg; }
    if (s_guard()) return true;
    s_pending.set = false;
    return false;
}
static void nav_push(ui_screen_create_fn create, void *arg);
static void nav_replace(ui_screen_create_fn create, void *arg);
static void nav_back(void);
static void nav_home(void);
void ui_nav_proceed(void)
{
    bool set = s_pending.set;
    nav_op_t op = s_pending.op;
    ui_screen_create_fn create = s_pending.create;
    void *arg = s_pending.arg;
    s_pending.set = false;
    if (s_guard_scr) lv_obj_remove_event_cb(s_guard_scr, guard_scr_deleted);
    s_guard_scr = NULL;   /* the guard said go: no second question */
    s_guard = NULL;
    if (!set || op == NAV_BACK) nav_back();
    else if (op == NAV_HOME) nav_home();
    else if (op == NAV_PUSH) nav_push(create, arg);
    else nav_replace(create, arg);
}
void ui_nav_cancel_pending(void) { s_pending.set = false; }

static void nav_push(ui_screen_create_fn create, void *arg)
{
    if (s_depth >= NAV_MAX) { ESP_LOGW(TAG, "nav stack full"); return; }
    s_stack[s_depth].create = create;
    s_stack[s_depth].arg = arg;
    s_depth++;
    nav_show_top();
}
void ui_nav_push(ui_screen_create_fn create, void *arg) { if (!guarded(NAV_PUSH, create, arg)) nav_push(create, arg); }

static void nav_replace(ui_screen_create_fn create, void *arg)
{
    if (s_depth == 0) s_depth = 1;
    s_stack[s_depth - 1].create = create;
    s_stack[s_depth - 1].arg = arg;
    nav_show_top();
}
void ui_nav_replace(ui_screen_create_fn create, void *arg) { if (!guarded(NAV_REPLACE, create, arg)) nav_replace(create, arg); }

static void nav_back(void)
{
    if (s_depth <= 1) { nav_home(); return; }
    s_depth--;
    nav_show_top();
}
void ui_nav_back(void) { if (!guarded(NAV_BACK, NULL, NULL)) nav_back(); }

static void nav_home(void)
{
    s_depth = 1;
    s_stack[0].create = ui_sessions_create;   /* Sessions is the only home */
    s_stack[0].arg = NULL;
    nav_show_top();
}
void ui_nav_home(void) { if (!guarded(NAV_HOME, NULL, NULL)) nav_home(); }

bool ui_nav_is_top(ui_screen_create_fn create)
{
    return s_depth > 0 && s_stack[s_depth - 1].create == create;
}

lv_obj_t *ui_nav_current(void) { return s_current; }
void ui_nav_refresh(void) { nav_show_top(); }

/* ------------------------------------------------------------ widgets */
lv_obj_t *ui_screen_base(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_comp_screen(scr);
    lv_obj_set_style_text_font(scr, FONT_BODY, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data)
{
    return ui_btn(parent, UI_BTN_SECONDARY, text, cb, user_data);
}

lv_obj_t *ui_row(lv_obj_t *parent)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_pad_all(r, 0, 0);
    lv_obj_set_style_pad_column(r, 10, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

lv_obj_t *ui_textarea(lv_obj_t *parent, const char *placeholder, bool password, bool oneline)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, LV_PCT(100));
    lv_textarea_set_one_line(ta, oneline);
    lv_textarea_set_password_mode(ta, password);
    lv_textarea_set_placeholder_text(ta, placeholder ? placeholder : "");
    lv_obj_set_style_bg_color(ta, lv_color_hex(UI_TK_BG), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(UI_TK_FG), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(UI_TK_PANEL2), 0);
    lv_obj_set_style_radius(ta, UI_TK_RADIUS, 0);
    return ta;
}

lv_obj_t *ui_switch_row(lv_obj_t *parent, const char *label, bool on, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(r, lv_color_hex(UI_TK_PANEL), 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_radius(r, UI_TK_RADIUS, 0);
    lv_obj_set_style_pad_all(r, 14, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_width(l, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(l, 1);
    lv_obj_t *sw = lv_switch_create(r);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user_data);
    lv_obj_set_user_data(r, sw);
    return r;
}

/* ------------------------------------------------------------- toast */
static lv_obj_t *s_toast = NULL;
static lv_timer_t *s_toast_timer = NULL;   /* one timer: an older toast's must not cut a newer one short */
/* A toast deleted by anything else (a cleaned layer) leaves no dangling pointer or timer behind. */
static void toast_deleted_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) != s_toast) return;
    s_toast = NULL;
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
}
static void toast_del_cb(lv_timer_t *t)
{
    if (s_toast_timer == t) s_toast_timer = NULL;
    lv_obj_t *o = s_toast;
    s_toast = NULL;   /* before the delete, so toast_deleted_cb leaves this timer alone */
    if (o) lv_obj_delete(o);
    lv_timer_delete(t);
}
/* ---------------------------------------------------- sleep notice */
static lv_obj_t *s_sleep_notice;

static void sleep_notice_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_sleep_notice) s_sleep_notice = NULL;
}

void ui_sleep_notice(bool show)
{
    if (!show) {
        if (s_sleep_notice) lv_obj_delete(s_sleep_notice);
        s_sleep_notice = NULL;
        return;
    }
    if (s_sleep_notice) return;
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, LV_PCT(100), LV_PCT(100));
    ui_comp_screen(o);   /* bg, fg */
    lv_obj_add_flag(o, UI_KEYBOARD_SKIP);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(o);
    lv_label_set_text(l, tr(STR_SLEEP_NOTICE));
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(l, LV_PCT(80));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_center(l);
    lv_obj_add_event_cb(o, sleep_notice_deleted, LV_EVENT_DELETE, NULL);
    s_sleep_notice = o;
}

void ui_toast(const char *text)
{
    ui_toast_ms(text, 3000);
}

/* Spec §5.13: one line, panel and fg, cut with dots if it is too long. Built here, placed by the caller. */
static void toast_build(const char *text)
{
    ESP_LOGI(TAG, "toast: %s", text);
    if (s_toast_timer) { lv_timer_delete(s_toast_timer); s_toast_timer = NULL; }
    if (s_toast) { lv_obj_delete(s_toast); s_toast = NULL; }
    s_toast = lv_obj_create(lv_layer_top());
    lv_obj_add_event_cb(s_toast, toast_deleted_cb, LV_EVENT_DELETE, NULL);   /* whoever deletes it */
    lv_obj_remove_style_all(s_toast);
    ui_comp_sheet(s_toast);
    lv_obj_set_size(s_toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(s_toast, UI_TK_RADIUS, 0);
    lv_obj_set_style_pad_hor(s_toast, UI_TK_GAP_L, 0);
    lv_obj_set_style_pad_ver(s_toast, 12, 0);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(s_toast);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, FONT_BODY, 0);
    int32_t max_w = lv_display_get_horizontal_resolution(NULL) - 2 * UI_TK_MARGIN - 2 * UI_TK_GAP_L;
    lv_point_t sz;
    lv_text_get_size(&sz, lv_label_get_text(l), FONT_BODY, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    lv_obj_set_size(l, sz.x < max_w ? sz.x : max_w, lv_font_get_line_height(FONT_BODY));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
}

/* At the bottom centre, just above the footer. */
void ui_toast_ms(const char *text, uint32_t ms)
{
    toast_build(text);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(UI_TK_FOOTER_H + UI_TK_GAP_M));
    s_toast_timer = lv_timer_create(toast_del_cb, ms, NULL);
}

/* Centred, its bottom edge a gap above `above` (a shown object on screen), so a long-lived toast leaves the controls
   from there down free to see; where ui_toast_ms puts it when `above` is NULL, hidden or too high for it. */
void ui_toast_above_ms(const char *text, uint32_t ms, lv_obj_t *above)
{
    toast_build(text);
    int32_t y = -1;
    if (above && lv_obj_is_visible(above)) {
        lv_obj_update_layout(s_toast);
        lv_area_t a;
        lv_obj_get_coords(above, &a);
        y = a.y1 - UI_TK_GAP_M - lv_obj_get_height(s_toast);
    }
    if (y >= 0) lv_obj_align(s_toast, LV_ALIGN_TOP_MID, 0, y);
    else lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -(UI_TK_FOOTER_H + UI_TK_GAP_M));
    s_toast_timer = lv_timer_create(toast_del_cb, ms, NULL);
}
void ui_toastf(const char *fmt, ...)
{
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    ui_toast(buf);
}

/* ------------------------------------------------------------ dialogs */
typedef struct { ui_confirm_cb_t cb; void *ud; lv_obj_t *box; } confirm_ctx_t;
static void confirm_btn_cb(lv_event_t *e)
{
    confirm_ctx_t *c = lv_event_get_user_data(e);
    bool yes = (bool)(uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    lv_obj_t *box = c->box;
    ui_confirm_cb_t cb = c->cb; void *ud = c->ud;
    free(c);
    lv_obj_delete(box);
    if (cb) cb(yes, ud);
}

static void modal_del_cb(lv_event_t *e) { (void)e; if (s_modal_count) s_modal_count--; }

static lv_obj_t *modal_overlay(void)
{
    lv_obj_t *ov = lv_obj_create(lv_layer_top());
    s_modal_count++;
    lv_obj_add_event_cb(ov, modal_del_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(ov);
    lv_obj_set_size(ov, LV_PCT(100), LV_PCT(100));
    ui_comp_scrim(ov);
    lv_obj_remove_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ov, LV_OBJ_FLAG_CLICKABLE);
    return ov;
}
static lv_obj_t *modal_box(lv_obj_t *ov, const char *title)
{
    lv_obj_t *box = lv_obj_create(ov);
    lv_obj_remove_style_all(box);
    ui_comp_sheet(box);
    lv_obj_set_width(box, LV_PCT(60));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, LV_PCT(90), 0);
    lv_obj_center(box);
    lv_obj_set_style_radius(box, UI_TK_RADIUS, 0);
    lv_obj_set_style_pad_all(box, UI_TK_GAP_L, 0);
    lv_obj_set_style_pad_row(box, UI_TK_GAP_M, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    if (title) {
        /* the title wraps and never clips (spec §5.13) */
        lv_obj_t *t = lv_label_create(box);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, FONT_BODY, 0);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    }
    return box;
}

/* A right-aligned row for a dialog's buttons. */
static lv_obj_t *dialog_buttons(lv_obj_t *box)
{
    lv_obj_t *row = ui_row(box);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, UI_TK_GAP_M, 0);
    /* the buttons' 80 px hit area fits inside the row each way: a short end button (OK, Forget) keeps its
       right-hand 8 px (P4C-5; LVGL clips a child's hit area at its parent) */
    lv_obj_set_style_pad_ver(row, (UI_TK_HIT - UI_TK_TARGET) / 2, 0);
    lv_obj_set_style_pad_hor(row, (UI_TK_HIT - UI_TK_TARGET) / 2, 0);
    return row;
}

static lv_obj_t *dialog_body(lv_obj_t *box, const char *text) { return ui_comp_body(box, text); }

void ui_confirm_ex(const char *title, const char *body, const char *action, bool destructive,
                   ui_confirm_cb_t cb, void *ud)
{
    lv_obj_t *ov = modal_overlay();
    lv_obj_t *box = modal_box(ov, title);
    if (body) dialog_body(box, body);
    confirm_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->ud = ud; c->box = ov;
    lv_obj_t *row = dialog_buttons(box);
    lv_obj_t *no = ui_btn(row, UI_BTN_SECONDARY, tr(STR_CANCEL), confirm_btn_cb, c);
    lv_obj_add_event_cb(ov, confirm_btn_cb, LV_EVENT_CANCEL, c);
    lv_obj_set_user_data(no, (void *)0);
    lv_obj_t *yes = ui_btn(row, destructive ? UI_BTN_DESTRUCTIVE : UI_BTN_PRIMARY, action ? action : tr(STR_OK),
                           confirm_btn_cb, c);
    lv_obj_set_user_data(yes, (void *)1);
}

void ui_confirm(const char *title, const char *text, ui_confirm_cb_t cb, void *user_data)
{
    ui_confirm_ex(title, text, NULL, false, cb, user_data);
}

static void alert_ok_cb(lv_event_t *e) { lv_obj_delete(lv_event_get_user_data(e)); }

void ui_sheet_text(const char *title, const char *mono_text, const char *note)
{
    lv_obj_t *ov = modal_overlay();
    lv_obj_t *box = modal_box(ov, title);
    lv_obj_set_width(box, LV_PCT(70));
    lv_obj_t *m = ui_text(box, mono_text, UI_TONE_FG, false);
    ui_text_mono(m);
    lv_obj_set_width(m, LV_PCT(100));
    lv_label_set_long_mode(m, LV_LABEL_LONG_MODE_WRAP);
    if (note) dialog_body(box, note);
    ui_btn(dialog_buttons(box), UI_BTN_PRIMARY, tr(STR_OK), alert_ok_cb, ov);
    lv_obj_add_event_cb(ov, alert_ok_cb, LV_EVENT_CANCEL, ov);
}
void ui_alert(const char *title, const char *text)
{
    lv_obj_t *ov = modal_overlay();
    lv_obj_t *box = modal_box(ov, title);
    if (text) dialog_body(box, text);
    ui_btn(dialog_buttons(box), UI_BTN_PRIMARY, tr(STR_OK), alert_ok_cb, ov);
    lv_obj_add_event_cb(ov, alert_ok_cb, LV_EVENT_CANCEL, ov);
}

typedef struct { ui_prompt_cb_t cb; void *ud; lv_obj_t *ov; lv_obj_t *ta; } prompt_ctx_t;
static void prompt_ok_cb(lv_event_t *e)
{
    prompt_ctx_t *c = lv_event_get_user_data(e);
    char *txt = strdup(lv_textarea_get_text(c->ta));
    ui_prompt_cb_t cb = c->cb; void *ud = c->ud; lv_obj_t *ov = c->ov;
    free(c);
    lv_obj_delete(ov);
    if (cb) cb(txt, ud);
    free(txt);
}
static void prompt_cancel_cb(lv_event_t *e)
{
    prompt_ctx_t *c = lv_event_get_user_data(e);
    lv_obj_t *ov = c->ov; free(c); lv_obj_delete(ov);
}
extern void softkbd_attach_textarea(lv_obj_t *ta, lv_obj_t *parent);  /* ui_softkbd.c */
/* A secret (password) is typed masked. The soft keyboard comes up only with no physical keyboard
 * present: softkbd_attach_textarea checks keyboard_any_present() each time the field is focused. */
void ui_prompt(const char *title, const char *placeholder, const char *initial, bool password, ui_prompt_cb_t cb, void *user_data)
{
    lv_obj_t *ov = modal_overlay();
    lv_obj_t *box = modal_box(ov, title);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, UI_TK_GAP_L);
    lv_obj_t *ta = ui_textarea(box, placeholder, password, true);
    if (initial) lv_textarea_set_text(ta, initial);
    prompt_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->ud = user_data; c->ov = ov; c->ta = ta;
    lv_obj_t *row = dialog_buttons(box);
    ui_btn(row, UI_BTN_SECONDARY, tr(STR_CANCEL), prompt_cancel_cb, c);
    lv_obj_add_event_cb(ov, prompt_cancel_cb, LV_EVENT_CANCEL, c);
    ui_btn(row, UI_BTN_PRIMARY, tr(STR_OK), prompt_ok_cb, c);
    softkbd_attach_textarea(ta, ov);
    /* Enter in the one-line field presses OK: the keyboard sink (and the soft keyboard's Enter) sends it READY.
       Added after the soft keyboard's handler, so that keyboard is hidden (and freed) before OK deletes the prompt;
       the soft keyboard's key handler, still on the stack, checks it wasn't freed before touching it again
       (ui_softkbd.c send_special, fix round 1 M6). */
    lv_obj_add_event_cb(ta, prompt_ok_cb, LV_EVENT_READY, c);
    lv_obj_add_state(ta, LV_STATE_FOCUSED);
}

typedef struct { ui_menu_cb_t cb; void *ud; lv_obj_t *ov; } menu_ctx_t;
/* M1 (final review): the open picker that has no current row (current < 0). Its first Enter or Space only lands on a
   row, so a stray Enter never picks row 0 (the Host picker would switch the Host). */
static lv_obj_t *s_fresh_picker;
static void menu_item_cb(lv_event_t *e)
{
    menu_ctx_t *c = lv_event_get_user_data(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target_obj(e));
    ui_menu_cb_t cb = c->cb; void *ud = c->ud; lv_obj_t *ov = c->ov;
    lv_obj_delete(ov);
    /* ctx freed in ov delete cb */
    if (cb) cb(idx, ud);
}
static void menu_ov_cb(lv_event_t *e)
{
    menu_ctx_t *c = lv_event_get_user_data(e);
    if (lv_event_get_code(e) == LV_EVENT_CLICKED || lv_event_get_code(e) == LV_EVENT_CANCEL) {
        if (lv_event_get_target_obj(e) == c->ov) lv_obj_delete(c->ov);
    } else if (lv_event_get_code(e) == LV_EVENT_DELETE) {
        if (s_fresh_picker == c->ov) s_fresh_picker = NULL;
        free(c);
    }
}

/* A sheet of 64 px rows: radio rows (the current one has an fg dot, the others a line ring), or plain action
 * rows whose label is red where `red` has the item's bit. */
static lv_obj_t *sheet_rows(const char *title, const char *const *items, int count, int current, bool radio,
                            uint32_t red, ui_menu_cb_t cb, void *ud)
{
    lv_obj_t *ov = modal_overlay();
    lv_obj_t *box = modal_box(ov, title);
    lv_obj_set_width(box, LV_PCT(50));
    lv_obj_set_style_pad_row(box, UI_TK_GAP_S, 0);
    menu_ctx_t *c = calloc(1, sizeof(*c));
    c->cb = cb; c->ud = ud; c->ov = ov;
    lv_obj_add_event_cb(ov, menu_ov_cb, LV_EVENT_CLICKED, c);
    lv_obj_add_event_cb(ov, menu_ov_cb, LV_EVENT_DELETE, c);
    lv_obj_add_event_cb(ov, menu_ov_cb, LV_EVENT_CANCEL, c);
    lv_obj_add_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(box, LV_DIR_VER);
    lv_obj_t *cur = NULL;
    for (int i = 0; i < count; i++) {
        lv_obj_t *r = ui_srow(box, UI_ROW_ACTION, items[i], NULL, menu_item_cb, c);
        lv_obj_set_user_data(r, (void *)(intptr_t)i);
        if (radio) lv_obj_move_to_index(ui_comp_radio_dot(r, i == current), 0);
        else if (i < 32 && (red >> i) & 1u) ui_text_tone(lv_obj_get_child(r, 0), UI_TONE_RED);
        if (i == current) { cur = r; lv_obj_add_flag(r, UI_KEYBOARD_FIRST); }
    }
    if (radio && !cur) s_fresh_picker = ov;
    if (cur) {
        lv_obj_update_layout(box);
        lv_obj_scroll_to_view(cur, LV_ANIM_OFF);
    }
    return box;
}

lv_obj_t *ui_picker(const char *title, const char *const *items, int count, int current, ui_menu_cb_t cb, void *ud)
{
    return sheet_rows(title, items, count, current, true, 0, cb, ud);
}

lv_obj_t *ui_action_sheet(const char *title, const char *const *items, int count, uint32_t destructive,
                          ui_menu_cb_t cb, void *ud)
{
    return sheet_rows(title, items, count, -1, false, destructive, cb, ud);
}

void ui_menu(const char *title, const char *const *items, int count, ui_menu_cb_t cb, void *user_data)
{
    ui_picker(title, items, count, -1, cb, user_data);
}

/* A spinner and a muted label. */
lv_obj_t *ui_busy(const char *text)
{
    lv_obj_t *ov = modal_overlay();
    lv_obj_t *box = modal_box(ov, NULL);
    lv_obj_set_width(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, UI_TK_GAP_M, 0);
    lv_obj_t *sp = lv_spinner_create(box);
    lv_obj_set_size(sp, 40, 40);
    lv_obj_t *l = lv_label_create(box);
    lv_label_set_text(l, text ? text : tr(STR_PLEASE_WAIT));
    ui_comp_busy(sp, l);
    lv_obj_set_user_data(ov, l);
    return ov;
}
void ui_busy_close(lv_obj_t *busy) { if (busy) lv_obj_delete(busy); }

/* Keyboard navigation walks the current object tree on each key. This includes
 * dynamically populated lists and hidden settings panels without stale groups.
 * Only the topmost modal is reachable while a dialog is open. */
static lv_obj_t *s_keyboard_focus;

/* The footer hint (spec §5.13): shown while the last input was a key, hidden by a touch. No timer. */
static lv_obj_t *s_key_hint;
static bool s_key_last;

static void key_hint_show(bool on)
{
    if (!s_key_hint || on != lv_obj_has_flag(s_key_hint, LV_OBJ_FLAG_HIDDEN)) return;
    if (on) lv_obj_remove_flag(s_key_hint, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_key_hint, LV_OBJ_FLAG_HIDDEN);
}

static void key_hint_deleted(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_key_hint) s_key_hint = NULL;
}

void ui_keyboard_hint(lv_obj_t *label)
{
    s_key_hint = label;
    if (!label) return;
    lv_obj_add_event_cb(label, key_hint_deleted, LV_EVENT_DELETE, NULL);
    if (s_key_last) lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
}

static void indev_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_key_last = false;
    key_hint_show(false);
}

static bool has_handler(lv_obj_t *obj, lv_event_code_t code)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(obj); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(obj, i);
        if (d->filter == (uint32_t)code) return true;
    }
    return false;
}

static lv_obj_t *keyboard_modal(void)
{
    lv_obj_t *top = lv_layer_top();
    for (int i = (int)lv_obj_get_child_count(top) - 1; i >= 0; i--) {
        lv_obj_t *obj = lv_obj_get_child(top, i);
        for (uint32_t j = 0; j < lv_obj_get_event_count(obj); j++)
            if (lv_event_dsc_get_cb(lv_obj_get_event_dsc(obj, j)) == modal_del_cb)
                return obj;
    }
    return NULL;
}

bool ui_keyboard_owns_input(void)
{
    return s_keyboard_terminal_ui || s_modal_count > 0;
}

static void keyboard_focus_deleted(lv_event_t *e)
{
    if (s_keyboard_focus == lv_event_get_target_obj(e)) {
        if (keyboard_lvgl_focused() == s_keyboard_focus) keyboard_lvgl_focus(NULL);
        s_keyboard_focus = NULL;
    }
}

static void keyboard_focus_set(lv_obj_t *obj)
{
    if (obj == s_keyboard_focus) return;
    lv_obj_t *old = s_keyboard_focus;
    s_keyboard_focus = NULL;
    if (old) {
        lv_obj_remove_event_cb(old, keyboard_focus_deleted);
        ui_comp_focus_ring(old, false);
        lv_obj_remove_state(old, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
        lv_obj_send_event(old, LV_EVENT_DEFOCUSED, NULL);
    }
    keyboard_lvgl_focus(NULL);
    s_keyboard_focus = obj;
    if (!obj) return;
    lv_obj_add_event_cb(obj, keyboard_focus_deleted, LV_EVENT_DELETE, NULL);
    /* a 2 px fg ring with a 2 px gap, never a hue (spec §8.1) */
    ui_comp_focus_ring(obj, true);
    lv_obj_add_state(obj, LV_STATE_FOCUSED | LV_STATE_FOCUS_KEY);
    lv_obj_send_event(obj, LV_EVENT_FOCUSED, NULL);
    if (lv_obj_check_type(obj, &lv_textarea_class)) keyboard_lvgl_focus(obj);
    lv_obj_scroll_to_view_recursive(obj, LV_ANIM_OFF);
}

typedef struct {
    lv_obj_t *first, *last, *previous, *next;
    lv_obj_t *preferred;   /* the first control flagged UI_KEYBOARD_FIRST */
    lv_obj_t *root;   /* an open dialog's scrim: never a control itself (a tap on it closes a sheet; a key must not) */
    bool found;
} keyboard_walk_t;

static bool keyboard_skipped(lv_obj_t *obj)
{
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(obj, UI_KEYBOARD_SKIP) || lv_obj_has_state(obj, LV_STATE_DISABLED);
}

static bool keyboard_control(lv_obj_t *obj)
{
    return lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE) &&
        (has_handler(obj, LV_EVENT_CLICKED) || has_handler(obj, LV_EVENT_SHORT_CLICKED) || has_handler(obj, LV_EVENT_LONG_PRESSED) || has_handler(obj, LV_EVENT_KEY) ||
         lv_obj_check_type(obj, &lv_textarea_class) || lv_obj_check_type(obj, &lv_slider_class) ||
         lv_obj_has_flag(obj, LV_OBJ_FLAG_CHECKABLE));
}

static void keyboard_walk(lv_obj_t *obj, keyboard_walk_t *walk)
{
    if (keyboard_skipped(obj)) return;
    bool control = keyboard_control(obj);
    if (control && obj != walk->root) {
        if (!walk->first) walk->first = obj;
        if (!walk->preferred && lv_obj_has_flag(obj, UI_KEYBOARD_FIRST)) walk->preferred = obj;
        if (obj == s_keyboard_focus) { walk->found = true; walk->previous = walk->last; }
        else if (walk->found && !walk->next) walk->next = obj;
        walk->last = obj;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++)
        keyboard_walk(lv_obj_get_child(obj, i), walk);
}

lv_obj_t *ui_keyboard_focused(void) { return s_keyboard_focus; }

/* The controls under obj in walk order: *n counts them; the one numbered want goes to *hit, and the focused one's
   number to *at. */
static void keyboard_nth(lv_obj_t *obj, int want, int *n, lv_obj_t **hit, int *at)
{
    if (keyboard_skipped(obj)) return;
    if (keyboard_control(obj)) {
        if (*n == want && !*hit) *hit = obj;
        if (obj == s_keyboard_focus) *at = *n;
        (*n)++;
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); i++) keyboard_nth(lv_obj_get_child(obj, i), want, n, hit, at);
}

int ui_keyboard_focus_index(lv_obj_t *root)
{
    int n = 0, at = -1;
    lv_obj_t *hit = NULL;
    if (root && s_keyboard_focus) keyboard_nth(root, -1, &n, &hit, &at);
    return at;
}

lv_obj_t *ui_keyboard_focus_in(lv_obj_t *root, int nth)
{
    if (!root || nth < 0) return NULL;
    int n = 0, at = -1;
    lv_obj_t *hit = NULL;
    keyboard_nth(root, nth, &n, &hit, &at);
    if (hit) keyboard_focus_set(hit);
    return hit;
}

bool ui_keyboard_route(const key_event_t *ev, bool terminal)
{
    lv_display_trigger_activity(NULL);
    s_key_last = true;
    key_hint_show(true);
    lv_obj_t *modal = keyboard_modal();
    if (!terminal) s_keyboard_terminal_ui = false;
    /* The Sessions Ctrl+K overlay: every key, Ctrl+Tab included, goes to its sink, which closes it. */
    if (terminal && !modal && ui_sessions_keys_overlay_shown()) { s_keyboard_terminal_ui = false; return false; }
    if (terminal && !modal && ev->type == KEY_EV_SPECIAL && ev->key == SK_TAB && (ev->mods & MOD_CTRL)) {
        s_keyboard_terminal_ui = !s_keyboard_terminal_ui;
        keyboard_focus_set(NULL);   /* no toast: the footer hint shows the keys (spec §5.13) */
        if (!s_keyboard_terminal_ui) return true;
    } else if (terminal && !modal && !s_keyboard_terminal_ui) return false;

    lv_obj_t *scope = modal ? modal : lv_screen_active();
    lv_obj_t *touched_ta = keyboard_lvgl_focused();
    if (scope != s_keyboard_scope) {
        keyboard_focus_set(NULL);
        s_keyboard_scope = scope;
    }
    /* Adopt a textarea selected by touch, only if it belongs to this scope. */
    if (touched_ta && touched_ta != s_keyboard_focus) {
        lv_obj_t *p = touched_ta;
        while (p && p != scope && !lv_obj_has_flag(p, LV_OBJ_FLAG_HIDDEN) &&
               !lv_obj_has_state(p, LV_STATE_DISABLED)) p = lv_obj_get_parent(p);
        if (p == scope) keyboard_focus_set(touched_ta);
    }
    keyboard_walk_t walk = { .root = modal };
    keyboard_walk(scope, &walk);
    bool had_focus = walk.found;
    lv_obj_t *start = walk.preferred ? walk.preferred : walk.first;   /* where the first key lands */
    if (!had_focus) keyboard_focus_set(start);
    lv_obj_t *obj = s_keyboard_focus;
    bool textarea = obj && lv_obj_check_type(obj, &lv_textarea_class);
    if (ev->type == KEY_EV_SPECIAL && ev->key == SK_ESC) {
        if (modal) lv_obj_send_event(modal, LV_EVENT_CANCEL, NULL);
        else if (terminal) { keyboard_focus_set(NULL); s_keyboard_terminal_ui = false; }
        else ui_nav_back();
        return true;
    }
    if (ev->type == KEY_EV_SPECIAL && ev->key == SK_TAB) {
        bool back = ev->mods & MOD_SHIFT;
        keyboard_focus_set(had_focus ? (back ? (walk.previous ? walk.previous : walk.last) :
                                             (walk.next ? walk.next : walk.first)) :
                                      (back ? walk.last : start));
        return true;
    }
    if (!obj) return true;
    bool menu = (ev->type == KEY_EV_SPECIAL && ev->key == SK_ENTER && (ev->mods & MOD_CTRL)) ||
                (ev->type == KEY_EV_SPECIAL && ev->key == SK_F10 && (ev->mods & MOD_SHIFT)) ||
                (ev->type == KEY_EV_CHAR && (ev->cp == 'm' || ev->cp == 'M') &&
                 (ev->mods & (MOD_CTRL | MOD_SHIFT)) == (MOD_CTRL | MOD_SHIFT));
    if (menu) { lv_obj_send_event(obj, LV_EVENT_LONG_PRESSED, NULL); return true; }
    if (textarea) return false; /* text and cursor keys pass through to the textarea */
    if (ev->type == KEY_EV_SPECIAL) {
        int direction = (ev->key == SK_LEFT || ev->key == SK_DOWN) ? -1 : 1;
        bool arrow = ev->key == SK_LEFT || ev->key == SK_RIGHT || ev->key == SK_UP || ev->key == SK_DOWN;
        if (arrow && lv_obj_check_type(obj, &lv_slider_class)) {
            lv_slider_set_value(obj, lv_slider_get_value(obj) + direction * ((ev->mods & MOD_SHIFT) ? 10 : 1), LV_ANIM_OFF);
            if (lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL) == LV_RESULT_OK)
                lv_obj_send_event(obj, LV_EVENT_RELEASED, NULL);
            return true;
        }
        if (arrow && has_handler(obj, LV_EVENT_KEY)) {
            uint32_t key = ev->key == SK_LEFT ? LV_KEY_LEFT : ev->key == SK_RIGHT ? LV_KEY_RIGHT :
                           ev->key == SK_UP ? LV_KEY_UP : LV_KEY_DOWN;
            lv_obj_send_event(obj, LV_EVENT_KEY, &key);
            return true;
        }
        if (arrow) {
            bool back = ev->key == SK_LEFT || ev->key == SK_UP;
            if (!had_focus) keyboard_focus_set(back ? walk.last : start);
            else keyboard_focus_set(back ? (walk.previous ? walk.previous : walk.last) : (walk.next ? walk.next : walk.first));
            return true;
        }
        if (ev->key == SK_PGUP || ev->key == SK_PGDN) {
            lv_obj_t *p = lv_obj_get_parent(obj);
            while (p && !lv_obj_has_flag(p, LV_OBJ_FLAG_SCROLLABLE)) p = lv_obj_get_parent(p);
            if (p) lv_obj_scroll_by(p, 0, (ev->key == SK_PGUP ? 1 : -1) * lv_obj_get_height(p) * 3 / 4, LV_ANIM_OFF);
            return true;
        }
    }
    if ((ev->type == KEY_EV_SPECIAL && ev->key == SK_ENTER) ||
        (ev->type == KEY_EV_CHAR && ev->cp == ' ' && !ev->mods)) {
        if (!had_focus && modal && modal == s_fresh_picker) return true;   /* M1: lands on the first row only */
        if (lv_obj_has_flag(obj, LV_OBJ_FLAG_CHECKABLE)) {
            if (lv_obj_has_state(obj, LV_STATE_CHECKED)) lv_obj_remove_state(obj, LV_STATE_CHECKED);
            else lv_obj_add_state(obj, LV_STATE_CHECKED);
            if (lv_obj_send_event(obj, LV_EVENT_VALUE_CHANGED, NULL) != LV_RESULT_OK) return true;
        }
        /* Match a short pointer click, including tiles that intentionally use
         * SHORT_CLICKED to distinguish their long-press shortcut. */
        if (lv_obj_send_event(obj, LV_EVENT_SHORT_CLICKED, NULL) == LV_RESULT_OK)
            lv_obj_send_event(obj, LV_EVENT_CLICKED, NULL);
        return true;
    }
    return true;
}

/* ------------------------------------------------------------ gestures */
static lv_obj_t       *s_gesture_scr;
static ui_gesture_cb_t s_gesture_cb;

static void gesture_scr_del_cb(lv_event_t *e)
{
    if (lv_event_get_target_obj(e) == s_gesture_scr) { s_gesture_scr = NULL; s_gesture_cb = NULL; }
}

void ui_screen_set_gesture(lv_obj_t *scr, ui_gesture_cb_t cb)
{
    s_gesture_scr = scr;
    s_gesture_cb = cb;
    lv_obj_add_event_cb(scr, gesture_scr_del_cb, LV_EVENT_DELETE, NULL);
}

static void indev_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *in = lv_indev_active();
    if (!in || s_modal_count || power_shield_up()) return;   /* a swipe that wakes the screen does nothing else */
    /* A slider drag belongs to the control, never to screen navigation.
     * The indev callback bypasses LV_OBJ_FLAG_GESTURE_BUBBLE. */
    for (lv_obj_t *obj = lv_indev_get_active_obj(); obj; obj = lv_obj_get_parent(obj))
        if (lv_obj_check_type(obj, &lv_slider_class)) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(in);
    if (s_gesture_scr && s_gesture_scr == lv_screen_active()) { if (s_gesture_cb) s_gesture_cb(dir); return; }
    if (dir == LV_DIR_RIGHT) ui_nav_back();
}

/* ------------------------------------------------------- Tailscale mark */
lv_obj_t *ui_tailscale_icon(lv_obj_t *parent, int dot, int gap)
{
    int size = dot * 3 + gap * 2;
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, size, size);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    for (int y = 0; y < 3; y++) {
        for (int x = 0; x < 3; x++) {
            lv_obj_t *d = lv_obj_create(c);
            lv_obj_set_size(d, dot, dot);
            lv_obj_set_pos(d, x * (dot + gap), y * (dot + gap));
            lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(d, 0, 0);
            lv_obj_set_style_pad_all(d, 0, 0);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    return c;
}

void ui_tailscale_icon_set_color(lv_obj_t *icon, lv_color_t color, lv_opa_t opa)
{
    uint32_t n = lv_obj_get_child_count(icon);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *d = lv_obj_get_child(icon, i);
        /* only on a change: a style write redraws */
        if (!lv_color_eq(lv_obj_get_style_bg_color(d, 0), color)) lv_obj_set_style_bg_color(d, color, 0);
        if (lv_obj_get_style_bg_opa(d, 0) != opa) lv_obj_set_style_bg_opa(d, opa, 0);
    }
}

void ui_format_duration(char *buf, size_t len, uint32_t s)
{
    if (s >= 3600) snprintf(buf, len, "%u:%02u:%02u", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(buf, len, "%u:%02u", s / 60, s % 60);
}

const char *ui_basename(const char *path)
{
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

void ui_lock(void) { lvgl_port_lock(0); }
void ui_unlock(void) { lvgl_port_unlock(); }

typedef struct { lv_async_cb_t fn; void *arg; } async_t;
static void async_trampoline(void *p)
{
    async_t *a = p;
    a->fn(a->arg);
    free(a);
}
bool ui_async(lv_async_cb_t fn, void *arg)
{
    async_t *a = malloc(sizeof(*a));
    if (!a) return false;
    a->fn = fn; a->arg = arg;
    bool ok = false;
    if (lvgl_port_lock(UI_ASYNC_LOCK_MS)) {   /* never forever (I2) */
        ok = lv_async_call(async_trampoline, a) == LV_RESULT_OK;
        lvgl_port_unlock();
    }
    if (!ok) free(a);
    return ok;
}

static void timer_bind_del_cb(lv_event_t *e)
{
    lv_timer_t *t = lv_event_get_user_data(e);
    lv_timer_delete(t);
}
void ui_timer_bind(lv_timer_t *t, lv_obj_t *obj)
{
    lv_obj_add_event_cb(obj, timer_bind_del_cb, LV_EVENT_DELETE, t);
}

void ui_init(void)
{
    ui_comp_init();
    /* LVGL's default theme in the tokens, under ui_comp's layer (no shadows, no transitions) */
    lv_theme_t *th = lv_theme_default_init(NULL, lv_color_hex(UI_TK_FG), lv_color_hex(UI_TK_BG), true, FONT_BODY);
    ui_comp_install_theme(th);
    /* lv_display_set_theme() only applies the theme to the active screen, so the layers keep
     * LV_FONT_DEFAULT (Montserrat, no CJK); give them the UI font. */
    lv_obj_t *layers[] = { lv_layer_top(), lv_layer_sys(), lv_layer_bottom() };
    for (size_t i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
        lv_obj_set_style_text_font(layers[i], FONT_BODY, 0);
        lv_obj_set_style_text_color(layers[i], lv_color_hex(UI_TK_FG), 0);
    }
    lv_obj_set_style_bg_opa(lv_layer_top(), LV_OPA_TRANSP, 0);
    if (!s_blank) {
        s_blank = lv_obj_create(NULL);
        lv_obj_remove_style_all(s_blank);
        ui_comp_screen(s_blank);
    }
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in))
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER)
        {
            lv_indev_add_event_cb(in, indev_gesture_cb, LV_EVENT_GESTURE, NULL);
            lv_indev_add_event_cb(in, indev_pressed_cb, LV_EVENT_PRESSED, NULL);
        }
}
