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

typedef struct { str_id_t keys, what; } hk_line_t;   /* tr() ids */
extern const hk_line_t hk_overlay[];   /* the Ctrl+K overlay, one shortcut per line */
extern const int hk_overlay_n;
