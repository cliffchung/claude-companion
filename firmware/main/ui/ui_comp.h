#pragma once
/* The shared components (spec §8.2). Each is a function plus file-static lv_style_t styles that
 * ui_comp_init() builds once: a component's only local styles are geometry. That keeps LVGL's
 * small allocations, which land in internal RAM, to one style list entry per style.
 *
 * Fonts are chosen here and in fonts.c only (FONT_TITLE / FONT_BODY / FONT_SMALL / FONT_MONO), so a
 * later face change touches one place. */
#include "lvgl.h"
#include "ui/ui.h"
#include "ui/ui_tokens.h"
#include "companion/cc_sessions.h"
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

void ui_comp_init(void);

/* NEEDS YOU amber. Its only callers are ui_state_pill_set, ui_session_row_set and
 * ui_header_needs_you_set; each marks the amber object with UI_NEEDS_YOU_PART (ui_lint). */
lv_color_t ui_needs_you_color(void);

/* An object whose pixels may be amber (the preview's amber scan excludes its coords). */
#define UI_NEEDS_YOU_PART LV_OBJ_FLAG_USER_2
/* A control inside a 56/64 px bar: 48 px visible, an 80 px hit area. */
#define UI_BAR_CONTROL    LV_OBJ_FLAG_USER_3

/* ---- buttons: 64 px, hit area 80 ---- */
typedef enum { UI_BTN_PRIMARY, UI_BTN_SECONDARY, UI_BTN_DESTRUCTIVE } ui_btn_kind_t;
/* Primary: fg fill, bg text. Secondary: panel-2, line border. Destructive: red text, red border. */
lv_obj_t *ui_btn(lv_obj_t *parent, ui_btn_kind_t kind, const char *text, lv_event_cb_t cb, void *ud);
/* A bar chip: 48 px, panel-2, radius 8, hit area 80, UI_BAR_CONTROL. */
lv_obj_t *ui_chip(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);

/* ---- bars ---- */
/* 56 px, panel, a 1 px line underneath: [← back_label] title [needs you] [slot] vitals.
 * With back_label set, the "← <back_label>" bar control calls ui_nav_back. */
lv_obj_t *ui_header(lv_obj_t *scr, const char *title, const char *back_label);
lv_obj_t *ui_header_slot(lv_obj_t *header);           /* the flex slot between the title and the vitals */
/* Every bar control (header, footer and banner chips) hits the bar's full height and >= 80 px across (P4C-5). */
lv_obj_t *ui_header_needs_you(lv_obj_t *header);      /* the count label, hidden until set above 0 */
void ui_header_needs_you_set(lv_obj_t *label, int n); /* "● N need you" / "● 1 needs you"; hidden at 0 */

typedef enum { UI_BANNER_MUTED, UI_BANNER_RED } ui_banner_kind_t;
/* 48 px across the screen at y, under the header. Starts hidden. */
lv_obj_t *ui_banner(lv_obj_t *scr, int32_t y);
/* NULL text hides it. An action adds a bar chip that calls cb(ud). Styles are written only when
 * the kind changes, and text only when it differs. */
void ui_banner_set(lv_obj_t *banner, ui_banner_kind_t kind, const char *text, const char *action,
                   lv_event_cb_t cb, void *ud);

/* 64 px, panel, a 1 px line on top, at the bottom of scr; a left and a right flex group. */
lv_obj_t *ui_footer(lv_obj_t *scr);
lv_obj_t *ui_footer_group(lv_obj_t *footer, bool right);

/* ---- sessions ---- */
/* NEEDS YOU (only when cc_session_needs_you) is amber with bg text, WORKING green, the rest muted on line. */
lv_obj_t *ui_state_pill(lv_obj_t *parent);
void ui_state_pill_set(lv_obj_t *pill, const cc_session_t *s);
/* 72 px, radius 12, panel with a line border; the name (cut with "…"), meta and the state pill.
 * NEEDS YOU: a 4 px amber left edge and a 1 px amber border at 60 % (100 % selected).
 * Selected: panel-2 with a 2 px fg outline at 50 %. A set with unchanged content does nothing. */
lv_obj_t *ui_session_row(lv_obj_t *parent, lv_event_cb_t cb, void *ud);
void ui_session_row_set(lv_obj_t *row, const cc_session_t *s, bool selected, const char *meta);

/* ---- settings ---- */
typedef enum { UI_ROW_VALUE, UI_ROW_SWITCH, UI_ROW_NAV, UI_ROW_ACTION, UI_ROW_INFO } ui_row_kind_t;
/* 64 px, panel, radius 12; the whole row is the hit target (80 px). cb runs on LV_EVENT_CLICKED with ud,
 * except UI_ROW_SWITCH: a tap toggles the switch, and cb runs on its LV_EVENT_VALUE_CHANGED with the
 * switch as the target; the switch starts on when value is non-NULL and non-empty.
 * VALUE and NAV show value muted at the right (NAV adds "›"); INFO is not clickable and wraps value. */
lv_obj_t *ui_srow(lv_obj_t *parent, ui_row_kind_t kind, const char *label, const char *value,
                  lv_event_cb_t cb, void *ud);
/* A 64 px row with the label and a slider; on_release runs on LV_EVENT_RELEASED with the slider as the target. */
lv_obj_t *ui_srow_slider(lv_obj_t *parent, const char *label, int min, int max, int val,
                         lv_event_cb_t on_release, void *ud);
/* Styles o (an object with its styles removed) as a Settings column: 24 px top and bottom, 32 sideways, 8 between
 * rows, a vertical flex column that scrolls. Settings' panel and the Network sub-pages. */
void ui_settings_column(lv_obj_t *o);
/* A column rebuilt in place keeps the keyboard focus on its row (T13-3), by a stable key, never by object pointer:
 * the row's first label text, its count among the column's rows with that text, and the control's number in the
 * row. Save before cleaning the column, restore after filling it. A row that went gets the column's first control,
 * so the focus never falls back to the header's "← Sessions"; a focus outside the column (or none) is left alone. */
typedef struct { bool had; char text[64]; int nth, ctl; } ui_focus_key_t;
void ui_focus_save(lv_obj_t *column, ui_focus_key_t *k);
void ui_focus_restore(lv_obj_t *column, const ui_focus_key_t *k);
/* A 64 px line for a row (flex-grow it) and a trailing 64 px button, 8 px apart; both keep their 80 px hit. */
lv_obj_t *ui_srow_line(lv_obj_t *parent);
/* The row's value label (the switch of a UI_ROW_SWITCH row, the slider of a ui_srow_slider row). */
lv_obj_t *ui_srow_value(lv_obj_t *row);
/* Selection on a rail or list of rows: panel-2 with a 50 % fg outline. */
void ui_srow_set_selected(lv_obj_t *row, bool selected);
lv_obj_t *ui_section_header(lv_obj_t *parent, const char *text);   /* label muted, 24 above, 8 below */
/* A visual chip at the end of a row (Forget, Sync now): 48 px, radius 8, not a control of its own (the row is the
 * hit target). Destructive: red text and a red border; otherwise panel-2. */
lv_obj_t *ui_srow_chip(lv_obj_t *row, const char *text, bool destructive);
/* A Settings rail row: 72 px, transparent until selected (ui_srow_set_selected), the whole row the hit target. */
lv_obj_t *ui_rail_row(lv_obj_t *parent, const char *label, lv_event_cb_t cb, void *ud);
/* A 24 px filled circle in rgb (a keyboard colour swatch; never amber). */
lv_obj_t *ui_swatch(lv_obj_t *parent, uint32_t rgb);
lv_obj_t *ui_footnote(lv_obj_t *parent, const char *text);         /* label muted, wrapped */

/* ---- Terminal ---- */
/* A host row (spec §5.3): a 72 px line holding the row (flex-grown: a dot, the name in body, meta muted, then a
 * trailing label that shows hint while the row has the keyboard focus) and, with edit_cb, a 64 px secondary button
 * labelled edit 8 px to its right. The button is touch only (UI_KEYBOARD_SKIP): the row's LONG_PRESSED (Ctrl+Enter
 * from the keyboard) also runs edit_cb, so the arrows go row to row. Both get ud. Returns the row. */
lv_obj_t *ui_host_row(lv_obj_t *parent, const char *name, const char *meta, const char *hint,
                      lv_event_cb_t cb, lv_event_cb_t edit_cb, const char *edit, void *ud);
/* The row's dot: -1 none (a LAN host), 0 offline (line grey), 1 online (green). */
void ui_host_row_dot(lv_obj_t *row, int dot);
/* The trailing label: note in red (a connect failure), or busy (a spinner and note muted); NULL and !busy goes back
 * to hint on focus. */
void ui_host_row_note(lv_obj_t *row, const char *note, bool busy, const char *hint);
/* A Host key row: the whole fingerprint in mono, muted and wrapped (at least 64 px), and a drawn chip (chip, NULL for
 * none; destructive for Forget, plain for Trust this key); the whole row is the hit target. Settings › Host and the
 * host form. */
lv_obj_t *ui_srow_fingerprint(lv_obj_t *parent, const char *fp, const char *chip, bool destructive, lv_event_cb_t cb,
                              void *ud);
/* "+ Add host": 72 px, a line border, muted text; the whole row is the hit target. */
lv_obj_t *ui_add_row(lv_obj_t *parent, const char *text, lv_event_cb_t cb, void *ud);
/* A ui_chip (or a banner's action chip) in the primary style: fg fill, bg text (Save, Turn on). */
void ui_chip_set_primary(lv_obj_t *chip);

/* ---- text ---- */
typedef enum { UI_TONE_FG, UI_TONE_MUTED, UI_TONE_GREEN, UI_TONE_RED } ui_tone_t;
/* A label in a tone: body, or the label role when small. */
lv_obj_t *ui_text(lv_obj_t *parent, const char *text, ui_tone_t tone, bool small);
void ui_text_tone(lv_obj_t *label, ui_tone_t tone);   /* replaces the label's tone */
void ui_text_mono(lv_obj_t *label);                    /* FONT_MONO: fingerprints, keys */
lv_obj_t *ui_title(lv_obj_t *parent, const char *text);   /* the title role (a panel heading) */

/* ---- take-over and SSH session ---- */
/* The session bar (spec §5.2): 56 px, panel, a 1 px line underneath, at the top of scr; a flex row 16 px apart,
 * 24 px margins. Its chips (ui_chip) hit its full height. */
lv_obj_t *ui_session_bar(lv_obj_t *scr);
/* The bar's name (one line, cut with "…", at most 400 px) and its muted state text, which takes the rest of the
 * row; both shared styles (no local ones). */
lv_obj_t *ui_session_bar_name(lv_obj_t *bar, const char *name);
lv_obj_t *ui_session_bar_state(lv_obj_t *bar);
/* A screen body from under the header (and the banner, when shown) to the bottom of the screen: shared styles. */
void ui_body_place(lv_obj_t *body, bool banner);
/* A 12 px link dot: UI_TONE_GREEN, UI_TONE_RED, or muted for any other tone. A set with the same tone does nothing. */
lv_obj_t *ui_dot(lv_obj_t *parent);
void ui_dot_tone(lv_obj_t *dot, ui_tone_t tone);

/* ---- dialogs ---- */
/* A sheet of 64 px radio rows with an fg dot on `current` (-1: none). cb(index, ud) on a choice;
 * Esc or a tap outside closes it without a call. */
lv_obj_t *ui_picker(const char *title, const char *const *items, int count, int current, ui_menu_cb_t cb, void *ud);
/* (returns the sheet: its rows follow the title label in item order, so a caller can add a swatch to each) */
/* A sheet of 64 px action rows (no radio dots): cb(index, ud) on a tap; an item whose bit is set in
 * `destructive` reads red (its action still confirms). Esc or a tap outside closes it without a call. */
lv_obj_t *ui_action_sheet(const char *title, const char *const *items, int count, uint32_t destructive,
                          ui_menu_cb_t cb, void *ud);
/* The title wraps. Cancel is secondary; the action (NULL: OK) is primary, or destructive in red. */
void ui_confirm_ex(const char *title, const char *body, const char *action, bool destructive,
                   ui_confirm_cb_t cb, void *ud);
/* A sheet: the title, mono_text in fg mono wrapped in full (a key line), a muted note, then OK. */
void ui_sheet_text(const char *title, const char *mono_text, const char *note);

/* ---- states ---- */
/* A muted centred line, and an optional primary button below it. */
lv_obj_t *ui_empty_state(lv_obj_t *parent, const char *text, const char *action, lv_event_cb_t cb, void *ud);
lv_obj_t *ui_skeleton_row(lv_obj_t *parent);   /* a 72 px panel-2 placeholder row */

/* ---- for ui_core.c and ui_softkbd.c only; screens use the components above ---- */
void ui_comp_install_theme(lv_theme_t *base);   /* the token layer over LVGL's default theme */
void ui_comp_focus_ring(lv_obj_t *obj, bool on); /* the shared focus ring, at LV_STATE_FOCUS_KEY */
void ui_comp_screen(lv_obj_t *scr);              /* bg, fg text */
void ui_comp_sheet(lv_obj_t *obj);               /* panel, a line border, fg text: dialogs, toasts */
void ui_comp_scrim(lv_obj_t *ov);                /* the dim layer behind a dialog */
lv_obj_t *ui_comp_radio_dot(lv_obj_t *parent, bool on);   /* the picker's 20 px dot: fg on, a line ring off */
void ui_comp_busy(lv_obj_t *spinner, lv_obj_t *label);    /* line/fg arcs, a muted label */
lv_obj_t *ui_comp_body(lv_obj_t *parent, const char *text);  /* a dialog's body: muted, wrapped */
/* Lets a container's children keep their 80 px hit area past its edges (LVGL clips hits at the parent). Also used
 * by screens whose own containers need it (Sessions); a row with a trailing button uses ui_srow_line. */
void ui_comp_pass_hits(lv_obj_t *o);

#ifdef __cplusplus
}
#endif
