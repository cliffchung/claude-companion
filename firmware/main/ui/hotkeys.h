#pragma once
/* Two-thumb hotkeys (spec "Hotkeys for two thumbs"). Pure: no ESP-IDF or LVGL headers,
 * so it also builds in firmware/test/host. */
#include <stdbool.h>
#include "input/keyboard.h"

typedef enum { HK_T_NONE, HK_T_LEAVE, HK_T_SCROLL_UP, HK_T_SCROLL_DOWN } hk_term_t;
/* takeover only: Ctrl+] (KEY_EV_CHAR ']' with MOD_CTRL) and Ctrl+Q / Ctrl+q (the alias) -> LEAVE;
   Shift+Up -> SCROLL_UP and Shift+Down -> SCROLL_DOWN (takeover only: a plain SSH terminal keeps
   Shift+arrows for the remote side); Shift+PgUp / Shift+PgDn -> SCROLL_UP / SCROLL_DOWN in any
   terminal, as today; everything else -> NONE (sent to the Mac) */
hk_term_t hk_takeover(const key_event_t *ev, bool takeover);

typedef enum { HK_D_NONE, HK_D_OPEN, HK_D_SETTINGS, HK_D_TERMINAL, HK_D_KEYS, HK_D_LOCAL } hk_dash_t;
/* KEY_EV_CHAR with MOD_CTRL or MOD_META: o/O -> OPEN, p/P -> SETTINGS, m/M -> TERMINAL, k/K -> KEYS,
   any other character -> LOCAL (eaten: never typed into an answer field, never sent);
   anything else -> NONE (dash_key's existing handling) */
hk_dash_t hk_dashboard(const key_event_t *ev);

#define HK_DASH_HINT "Ctrl+O open \xc2\xb7 Ctrl+P settings \xc2\xb7 Ctrl+K keys"
#define HK_TAKEOVER_TOAST "Ctrl+] to leave"
#define HK_TAKEOVER_TOAST_MS 3000
extern const char *const hk_overlay_lines[];   /* NULL-terminated; the Ctrl+K overlay, one shortcut per line */
