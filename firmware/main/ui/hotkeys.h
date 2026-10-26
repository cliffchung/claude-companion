#pragma once
/* Two-thumb hotkeys (spec "Hotkeys for two thumbs"). Pure: no ESP-IDF or LVGL headers,
 * so it also builds in firmware/test/host. */
#include <stdbool.h>
#include "input/keyboard.h"
#include "i18n/i18n.h"

typedef enum { HK_T_NONE, HK_T_LEAVE, HK_T_SCROLL_UP, HK_T_SCROLL_DOWN, HK_T_VOICE } hk_term_t;
/* Ctrl+] (KEY_EV_CHAR ']' with MOD_CTRL) and Ctrl+Q / Ctrl+q (the alias) -> LEAVE in both modes;
   Shift+Up -> SCROLL_UP and Shift+Down -> SCROLL_DOWN (takeover only: a plain SSH terminal keeps
   Shift+arrows for the remote side); Ctrl+Up / Ctrl+Down (Ctrl+Shift too, not with Alt or Meta) -> SCROLL_UP /
   SCROLL_DOWN in both modes (the Tab5 keyboard's scroll keys: it has no Shift+arrow or PgUp); Shift+PgUp / Shift+PgDn -> SCROLL_UP / SCROLL_DOWN in any
   terminal, as today; Ctrl+Space -> VOICE in both modes (voice input in a session: taken, never sent as NUL;
   Ctrl+@ still sends NUL); everything else -> NONE (sent to the Host) */
hk_term_t hk_takeover(const key_event_t *ev, bool takeover);

typedef enum { HK_D_NONE, HK_D_OPEN, HK_D_SETTINGS, HK_D_TERMINAL, HK_D_KEYS, HK_D_VOICE, HK_D_STOP, HK_D_LOCAL } hk_dash_t;
/* KEY_EV_CHAR with MOD_CTRL or MOD_META: o/O -> OPEN, p/P -> SETTINGS, t/T and m/M -> TERMINAL,
   k/K -> KEYS, x/X -> STOP (stop the selected session, after a confirm), ' ' with Ctrl -> VOICE (Plan 5: Sessions starts or stops voice input; no space is typed), any
   other character -> LOCAL (eaten: never typed into an answer field, never sent); anything else -> NONE
   (dash_key's existing handling) */
hk_dash_t hk_dashboard(const key_event_t *ev);

/* The Ctrl+K overlay, one shortcut per line, in two sections: Sessions (hk_dashboard's keys and the screen's own
   Up / Down, Enter, Esc) and In a session (hk_takeover's). Each line names the key events it documents (`covers`:
   the type, the special key or the lower-case character, and the modifiers it needs; an event with more modifiers
   matches too, and on Sessions Meta counts as Ctrl). test_hotkeys sweeps every key: one that hk_dashboard or
   hk_takeover keeps must be covered by a line of its section, so a new key can't come without its help line. */
typedef enum { HK_SEC_SESSIONS, HK_SEC_SESSION } hk_sec_t;
#define HK_COVERS 4
typedef struct { key_ev_type_t type; uint32_t code; uint8_t mods; } hk_key_t;   /* code: cp (lower case) or special_key_t; type 0 + code 0 ends */
typedef struct { hk_sec_t sec; str_id_t keys, what; hk_key_t covers[HK_COVERS]; } hk_line_t;   /* tr() ids */
extern const hk_line_t hk_overlay[];
extern const int hk_overlay_n;
str_id_t hk_section_title(hk_sec_t sec);
/* True when line l documents ev (see covers above). */
bool hk_line_covers(const hk_line_t *l, const key_event_t *ev);
