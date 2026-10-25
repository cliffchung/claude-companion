#pragma once
#include "lvgl.h"
#include "i18n/i18n.h"
#include "fonts/fonts.h"
#include "ui/ui_tokens.h"
#include <stdbool.h>
#include "input/keyboard.h"
#ifdef __cplusplus
extern "C" {
#endif

/* Exclude a touch-only subtree from physical keyboard navigation. */
#define UI_KEYBOARD_SKIP LV_OBJ_FLAG_USER_4
/* The control the first key focuses (Down, Tab or the first Enter), instead of the first one: a picker's current
   row, so a bare Enter on a fresh picker changes nothing. */
#define UI_KEYBOARD_FIRST LV_OBJ_FLAG_USER_1

/* ---- screen stack ---- */
typedef lv_obj_t *(*ui_screen_create_fn)(void *arg);
void ui_init(void);
bool ui_keyboard_route(const key_event_t *ev, bool terminal);
bool ui_keyboard_owns_input(void);
/* A screen's footer hint ("↑↓ move · Enter choose · Esc back"): ui_core shows it while the last input was a
   key and hides it on a touch. One per screen; forgotten when the label is deleted. */
void ui_keyboard_hint(lv_obj_t *label);
/* The control with the keyboard focus (NULL: none), so a panel rebuilt in place can put it back on the same row
   (T13-3). ui_keyboard_focus_index: the focused control's number among the keyboard controls in root's subtree
   (root itself counts), -1 when the focus isn't there. ui_keyboard_focus_in focuses control number nth there and
   returns it, or NULL when there is none (the focus is then unchanged). ui_comp's ui_focus_save/restore use them. */
lv_obj_t *ui_keyboard_focused(void);
int ui_keyboard_focus_index(lv_obj_t *root);
lv_obj_t *ui_keyboard_focus_in(lv_obj_t *root, int nth);

/* Screen gestures. LVGL only walks up from the touched widget while every
 * object in between has LV_OBJ_FLAG_GESTURE_BUBBLE, which is brittle across
 * cards, lists and buttons, so gestures are taken from the input device and
 * dispatched by active screen instead.
 * Without a handler a screen gets the default: swipe right anywhere goes back.
 * A screen that wants the horizontal axis for itself installs its own. */
typedef void (*ui_gesture_cb_t)(lv_dir_t dir);
void ui_screen_set_gesture(lv_obj_t *scr, ui_gesture_cb_t cb);
void ui_nav_push(ui_screen_create_fn create, void *arg);
void ui_nav_replace(ui_screen_create_fn create, void *arg);
void ui_nav_back(void);
void ui_nav_home(void);
bool ui_nav_is_top(ui_screen_create_fn create);
lv_obj_t *ui_nav_current(void);
void ui_nav_refresh(void);   /* recreate current screen (e.g. after language change) */
/* scr's back guard: every way off scr while it shows (ui_nav_back: the back chip, Esc, a swipe; ui_nav_home: a
   wake, `ui home`; ui_nav_push / ui_nav_replace: hotkeys, console verbs) asks guard() first. True means it is asking
   ("Discard changes?"): the move is kept as pending until the screen calls ui_nav_proceed (do it, and drop the
   guard) or ui_nav_cancel_pending. One at a time; cleared when scr is deleted. */
typedef bool (*ui_back_guard_t)(void);
void ui_nav_set_back_guard(lv_obj_t *scr, ui_back_guard_t guard);
void ui_nav_proceed(void);
void ui_nav_cancel_pending(void);

/* ---- common widgets ---- */
lv_obj_t *ui_screen_base(void);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *user_data);   /* secondary */
lv_obj_t *ui_row(lv_obj_t *parent);
lv_obj_t *ui_textarea(lv_obj_t *parent, const char *placeholder, bool password, bool oneline);
lv_obj_t *ui_switch_row(lv_obj_t *parent, const char *label, bool on, lv_event_cb_t cb, void *user_data);

void ui_toast(const char *text);                  /* ui_toast_ms(text, 3000) */
void ui_toast_ms(const char *text, uint32_t ms);
void ui_toastf(const char *fmt, ...);
typedef void (*ui_confirm_cb_t)(bool yes, void *user_data);
void ui_confirm(const char *title, const char *text, ui_confirm_cb_t cb, void *user_data);
void ui_alert(const char *title, const char *text);
typedef void (*ui_prompt_cb_t)(const char *text, void *user_data);
void ui_prompt(const char *title, const char *placeholder, const char *initial, bool password, ui_prompt_cb_t cb, void *user_data);
typedef void (*ui_menu_cb_t)(int index, void *user_data);
void ui_menu(const char *title, const char *const *items, int count, ui_menu_cb_t cb, void *user_data);   /* ui_picker, current -1 */
lv_obj_t *ui_busy(const char *text);      /* returns overlay; delete to dismiss */
void ui_busy_close(lv_obj_t *busy);

/* The Tailscale mark is a 3x3 grid of dots and no font shipped here has it, so
 * it is drawn. `dot` is the diameter of one dot and `gap` the space between. */
lv_obj_t *ui_tailscale_icon(lv_obj_t *parent, int dot, int gap);
void ui_tailscale_icon_set_color(lv_obj_t *icon, lv_color_t color, lv_opa_t opa);

/* Utilities */
void ui_format_duration(char *buf, size_t len, uint32_t seconds);
const char *ui_basename(const char *path);
void ui_lock(void);
void ui_unlock(void);
/* Run fn on the LVGL task (safe from other tasks). False when it couldn't be queued (no memory, or the LVGL lock
   wasn't free within UI_ASYNC_LOCK_MS): fn will not run, so a caller that handed over arg still owns it, and a caller
   that set a "queued" flag clears it. The wait is bounded (I2) so a task holding a lock the LVGL task waits on can
   never hang both: it drops one update instead. */
#define UI_ASYNC_LOCK_MS 1000
bool ui_async(lv_async_cb_t fn, void *arg);
/* Delete the timer automatically when obj (usually the screen) is deleted. */
void ui_timer_bind(lv_timer_t *t, lv_obj_t *obj);

/* Screens */
/* Terminal, the host list (ui_hosts.c, spec §5.3); arg unused. The Host's entry is never listed. */
lv_obj_t *ui_hosts_create(void *arg);
/* An SSH Terminal connect to server index idx failed with msg: Terminal shows it in red on that host's row (by
   the entry's name, host and port) the next time it shows. */
void ui_hosts_note_failure(int idx, const char *msg);
lv_obj_t *ui_terminal_create(void *arg);         /* arg: server index */
/* The SSH Terminal refuses the companion's Host (no login shell there) with this toast: the Terminal footnote's
   first sentence. */
#define TERM_COMPANION_REFUSED tr(STR_HOSTS_NOT_LISTED)
/* Full-screen terminal running one command on a pty exec channel (never a login shell); title (the session's
   name) heads its session bar. "← Leave", Ctrl+] or Ctrl+Q leaves without sending anything; on_close runs once
   when the terminal closes. */
void ui_terminal_open_command(int server_idx, const char *cmd, const char *title, void (*on_close)(void *ctx), void *ctx);
bool ui_terminal_is_top(void);                   /* either terminal is the current screen */
lv_obj_t *ui_sessions_create(void *arg);         /* home: the Host's Claude Code sessions (ui_sessions.h) */
bool ui_sessions_keys_overlay_shown(void);       /* the Sessions Ctrl+K overlay is up */
lv_obj_t *ui_settings_create(void *arg);
/* The pre-Hibernate notice: a full-screen bg object on lv_layer_top with the centred fg text
   "Sleeping to save battery. Press the side button to wake." (power lights the panel first). */
void ui_sleep_notice(bool show);

#ifdef __cplusplus
}
#endif
