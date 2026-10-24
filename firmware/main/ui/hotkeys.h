#pragma once
/* Two-thumb hotkeys (spec "Hotkeys for two thumbs"). Pure: no ESP-IDF or LVGL headers,
 * so it also builds in firmware/test/host. */
#include <stdbool.h>
#include "input/keyboard.h"

typedef enum { HK_T_NONE, HK_T_LEAVE, HK_T_SCROLL_UP, HK_T_SCROLL_DOWN } hk_term_t;
/* Ctrl+] (KEY_EV_CHAR ']' with MOD_CTRL) and Ctrl+Q / Ctrl+q (the alias) -> LEAVE in both modes;
   Shift+Up -> SCROLL_UP and Shift+Down -> SCROLL_DOWN (takeover only: a plain SSH terminal keeps
   Shift+arrows for the remote side); Shift+PgUp / Shift+PgDn -> SCROLL_UP / SCROLL_DOWN in any
   terminal, as today; everything else -> NONE (sent to the Host) */
hk_term_t hk_takeover(const key_event_t *ev, bool takeover);

typedef enum { HK_D_NONE, HK_D_OPEN, HK_D_SETTINGS, HK_D_TERMINAL, HK_D_KEYS, HK_D_VOICE, HK_D_LOCAL } hk_dash_t;
/* KEY_EV_CHAR with MOD_CTRL or MOD_META: o/O -> OPEN, p/P -> SETTINGS, t/T and m/M -> TERMINAL,
   k/K -> KEYS, ' ' with Ctrl -> VOICE, any other character -> LOCAL (eaten: never typed into an
   answer field, never sent); anything else -> NONE (dash_key's existing handling) */
hk_dash_t hk_dashboard(const key_event_t *ev);

typedef struct { const char *keys, *what; } hk_line_t;
extern const hk_line_t hk_overlay[];   /* NULL-terminated (keys == NULL); the Ctrl+K overlay, one shortcut per line */

#define HK_VOICE_TOAST "Voice input comes in a later update"
