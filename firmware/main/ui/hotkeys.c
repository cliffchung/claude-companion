/* Two-thumb hotkeys: which keys the terminal and the dashboard keep for themselves.
 * Pure (input/keyboard.h only), host-tested in firmware/test/host/test_hotkeys.c. */
#include <stddef.h>
#include "ui/hotkeys.h"

static uint32_t lower(uint32_t cp)
{
    return (cp >= 'A' && cp <= 'Z') ? cp + ('a' - 'A') : cp;
}

hk_term_t hk_takeover(const key_event_t *ev, bool takeover)
{
    if (!ev) return HK_T_NONE;
    if (ev->type == KEY_EV_CHAR) {
        if ((ev->mods & MOD_CTRL) && (ev->cp == ']' || lower(ev->cp) == 'q'))
            return HK_T_LEAVE;
        return HK_T_NONE;
    }
    if (!(ev->mods & MOD_SHIFT)) return HK_T_NONE;
    switch (ev->key) {
    case SK_PGUP: return HK_T_SCROLL_UP;
    case SK_PGDN: return HK_T_SCROLL_DOWN;
    case SK_UP:   return takeover ? HK_T_SCROLL_UP : HK_T_NONE;
    case SK_DOWN: return takeover ? HK_T_SCROLL_DOWN : HK_T_NONE;
    default:      return HK_T_NONE;
    }
}

hk_dash_t hk_dashboard(const key_event_t *ev)
{
    if (!ev || ev->type != KEY_EV_CHAR || !(ev->mods & (MOD_CTRL | MOD_META))) return HK_D_NONE;
    if (ev->cp == ' ' && (ev->mods & MOD_CTRL)) return HK_D_VOICE;
    switch (lower(ev->cp)) {
    case 'o': return HK_D_OPEN;
    case 'p': return HK_D_SETTINGS;
    case 't':
    case 'm': return HK_D_TERMINAL;
    case 'k': return HK_D_KEYS;
    default:  return HK_D_LOCAL;
    }
}

const hk_line_t hk_overlay[] = {
    { "Up / Down", "move between sessions" },
    { "Enter", "send the answer" },
    { "Ctrl+O", "open the session" },
    { "Ctrl+T", "terminal (Ctrl+M also)" },
    { "Ctrl+P", "settings" },
    { "Ctrl+K", "this list" },
    { "Ctrl+Space", "voice input (coming later)" },
    { "Esc", "clear the answer, or go back" },
    { "Ctrl+]", "leave a take-over or SSH session" },
    { "Shift+Up / Down", "scroll back in a take-over" },
    { NULL, NULL },
};
