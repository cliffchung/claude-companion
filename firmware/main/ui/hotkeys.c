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
        if (takeover && (ev->mods & MOD_CTRL) && (ev->cp == ']' || lower(ev->cp) == 'q'))
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
    switch (lower(ev->cp)) {
    case 'o': return HK_D_OPEN;
    case 'p': return HK_D_SETTINGS;
    case 'm': return HK_D_MORE;
    case 'k': return HK_D_KEYS;
    default:  return HK_D_LOCAL;
    }
}

const char *const hk_overlay_lines[] = {
    "Up / Down  move between cards",
    "Enter  send the answer",
    "Ctrl+O  open the session",
    "Ctrl+P  settings",
    "Ctrl+M  more",
    "Ctrl+K  this list",
    "Esc  back (Settings, More)",
    "In a session: Ctrl+]  leave (it keeps running)",
    "In a session: Shift+Up / Down  scroll back",
    "Ctrl+Tab  buttons mode",
    NULL,
};
