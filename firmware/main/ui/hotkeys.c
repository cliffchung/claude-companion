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
        if ((ev->mods & MOD_CTRL) && ev->cp == ' ') return HK_T_VOICE;   /* voice in a session; Ctrl+@ still sends NUL */
        return HK_T_NONE;
    }
    /* Ctrl+← (the Tab5 keyboard's "<" key, one hand on each side) is the leave key; Ctrl+Q and Ctrl+] stay.
       The keyboard's own firmware drops Ctrl on "]", so Ctrl+] works only from a USB keyboard. */
    if (ev->key == SK_LEFT && (ev->mods & MOD_CTRL)) return HK_T_LEAVE;
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
    { STR_HK_K_UPDOWN, STR_HK_W_UPDOWN },
    { STR_HK_K_ENTER, STR_HK_W_ENTER },
    { STR_HK_K_CTRL_O, STR_HK_W_CTRL_O },
    { STR_HK_K_CTRL_T, STR_HK_W_CTRL_T },
    { STR_HK_K_CTRL_P, STR_HK_W_CTRL_P },
    { STR_HK_K_CTRL_K, STR_HK_W_CTRL_K },
    { STR_HK_K_CTRL_SPACE, STR_HK_W_CTRL_SPACE },
    { STR_HK_K_ESC, STR_HK_W_ESC },
    { STR_HK_K_CTRL_BRACKET, STR_HK_W_CTRL_BRACKET },
    { STR_HK_K_SHIFT_UPDOWN, STR_HK_W_SHIFT_UPDOWN },
};
const int hk_overlay_n = (int)(sizeof(hk_overlay) / sizeof(hk_overlay[0]));
