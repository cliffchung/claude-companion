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
    /* Ctrl+↑ / Ctrl+↓ scroll the view in both modes: the Tab5 keyboard has no Shift+arrow, Fn or PgUp/PgDn, but sends
       these intact. Alt+↑/↓ (and Ctrl+Alt) stay with the Host: Claude Code, shells and editors use them. */
    if ((ev->mods & MOD_CTRL) && !(ev->mods & (MOD_ALT | MOD_META))) {
        if (ev->key == SK_UP) return HK_T_SCROLL_UP;
        if (ev->key == SK_DOWN) return HK_T_SCROLL_DOWN;
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
    case 'x': return HK_D_STOP;
    default:  return HK_D_LOCAL;
    }
}

#define CH(c, m) { KEY_EV_CHAR, (c), (m) }
#define SP(k, m) { KEY_EV_SPECIAL, (k), (m) }
const hk_line_t hk_overlay[] = {
    { HK_SEC_SESSIONS, STR_HK_K_UPDOWN, STR_HK_W_UPDOWN, { SP(SK_UP, 0), SP(SK_DOWN, 0) } },
    { HK_SEC_SESSIONS, STR_HK_K_ENTER, STR_HK_W_ENTER, { SP(SK_ENTER, 0) } },
    { HK_SEC_SESSIONS, STR_HK_K_CTRL_O, STR_HK_W_CTRL_O, { CH('o', MOD_CTRL) } },
    { HK_SEC_SESSIONS, STR_HK_K_CTRL_X, STR_HK_W_CTRL_X, { CH('x', MOD_CTRL) } },
    { HK_SEC_SESSIONS, STR_HK_K_CTRL_T, STR_HK_W_CTRL_T, { CH('t', MOD_CTRL), CH('m', MOD_CTRL) } },
    { HK_SEC_SESSIONS, STR_HK_K_CTRL_P, STR_HK_W_CTRL_P, { CH('p', MOD_CTRL) } },
    { HK_SEC_SESSIONS, STR_HK_K_CTRL_K, STR_HK_W_CTRL_K, { CH('k', MOD_CTRL) } },
    { HK_SEC_SESSIONS, STR_HK_K_CTRL_SPACE, STR_HK_W_CTRL_SPACE, { CH(' ', MOD_CTRL) } },
    { HK_SEC_SESSIONS, STR_HK_K_ESC, STR_HK_W_ESC, { SP(SK_ESC, 0) } },
    { HK_SEC_SESSION, STR_HK_K_CTRL_BRACKET, STR_HK_W_CTRL_BRACKET, { SP(SK_LEFT, MOD_CTRL), CH('q', MOD_CTRL), CH(']', MOD_CTRL) } },
    { HK_SEC_SESSION, STR_HK_K_CTRL_UPDOWN, STR_HK_W_CTRL_UPDOWN, { SP(SK_UP, MOD_CTRL), SP(SK_DOWN, MOD_CTRL) } },
    { HK_SEC_SESSION, STR_HK_K_SHIFT_UPDOWN, STR_HK_W_SHIFT_UPDOWN,
      { SP(SK_UP, MOD_SHIFT), SP(SK_DOWN, MOD_SHIFT), SP(SK_PGUP, MOD_SHIFT), SP(SK_PGDN, MOD_SHIFT) } },
    { HK_SEC_SESSION, STR_HK_K_CTRL_SPACE, STR_HK_W_DICTATE, { CH(' ', MOD_CTRL) } },
};
#undef CH
#undef SP
const int hk_overlay_n = (int)(sizeof(hk_overlay) / sizeof(hk_overlay[0]));

str_id_t hk_section_title(hk_sec_t sec) { return sec == HK_SEC_SESSIONS ? STR_HK_SEC_SESSIONS : STR_HK_SEC_SESSION; }

bool hk_line_covers(const hk_line_t *l, const key_event_t *ev)
{
    if (!l || !ev) return false;
    uint8_t m = ev->mods;
    if (l->sec == HK_SEC_SESSIONS && (m & MOD_META)) m |= MOD_CTRL;   /* hk_dashboard takes Meta as Ctrl */
    uint32_t code = ev->type == KEY_EV_CHAR ? lower(ev->cp) : (uint32_t)ev->key;
    for (int i = 0; i < HK_COVERS; i++) {
        const hk_key_t *k = &l->covers[i];
        if (k->type == 0 && k->code == 0) break;
        if (k->type == ev->type && k->code == code && (m & k->mods) == k->mods) return true;
    }
    return false;
}
