/* `key log` line formatting; host-tested in firmware/test/host/test_key_log.c. */
#include "input/key_log.h"
#include <stdio.h>
#include <string.h>

static const char *sk_name(special_key_t k)
{
    static const char *const names[] = {
        "none", "enter", "esc", "backspace", "tab", "up", "down", "left", "right",
        "home", "end", "pgup", "pgdn", "insert", "delete",
        "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
        "ime-toggle", "kana-mode", "ime-ascii", "ime-kana",
    };
    return (unsigned)k < sizeof(names) / sizeof(names[0]) ? names[k] : "?";
}

void key_log_format(const key_event_t *ev, bool swallowed, char *out, size_t cap)
{
    char cp[16], mods[32] = "";
    if (ev->type == KEY_EV_CHAR && !(ev->mods & (MOD_CTRL | MOD_META | MOD_ALT)))
        strcpy(cp, "'*'");   /* typed text: masked */
    else if (ev->type == KEY_EV_CHAR && ev->cp >= 0x20 && ev->cp <= 0x7e)
        snprintf(cp, sizeof(cp), "'%c'", (char)ev->cp);
    else
        snprintf(cp, sizeof(cp), "0x%lx", (unsigned long)ev->cp);
    static const struct { uint8_t bit; const char *name; } m[] = {
        { MOD_CTRL, "ctrl" }, { MOD_SHIFT, "shift" }, { MOD_ALT, "alt" }, { MOD_META, "meta" },
    };
    for (size_t i = 0; i < sizeof(m) / sizeof(m[0]); i++) {
        if (!(ev->mods & m[i].bit)) continue;
        if (mods[0]) strncat(mods, ",", sizeof(mods) - strlen(mods) - 1);
        strncat(mods, m[i].name, sizeof(mods) - strlen(mods) - 1);
    }
    snprintf(out, cap, "key: type=%s cp=%s key=%s mods=%s%s", ev->type == KEY_EV_CHAR ? "char" : "special", cp,
             ev->type == KEY_EV_SPECIAL ? sk_name(ev->key) : "none", mods[0] ? mods : "none",
             swallowed ? " (wake key, swallowed)" : "");
}

bool key_log_expired(int64_t started_ms, int64_t now_ms)
{
    return now_ms - started_ms >= KEY_LOG_AUTO_OFF_MS;
}
