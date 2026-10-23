#include "keyboard.h"
#include "sys/debug_console.h"
#include "esp_console.h"
#include <stdio.h>
#include <string.h>
#include "ui/ui.h"

/* `key <name>` and `key text <string>`: inject events into the same queue the
 * A164 keyboard feeds, so the UI cannot tell them apart. */

static const struct { const char *name; special_key_t key; } s_names[] = {
    { "up", SK_UP }, { "down", SK_DOWN }, { "left", SK_LEFT }, { "right", SK_RIGHT },
    { "enter", SK_ENTER }, { "esc", SK_ESC }, { "tab", SK_TAB }, { "backspace", SK_BACKSPACE },
    { "pgup", SK_PGUP }, { "pgdn", SK_PGDN }, { "home", SK_HOME }, { "end", SK_END },
};

/* Posts a named special key with mods; false if the name is unknown. */
static bool post_named(const char *name, uint8_t mods, const char *prefix, int *rc)
{
    for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); i++) {
        if (strcmp(name, s_names[i].name) != 0) continue;
        key_event_t ev = { .type = KEY_EV_SPECIAL, .key = s_names[i].key, .mods = mods };
        if (!keyboard_post(&ev)) { printf("key: queue full\n"); *rc = 1; return true; }
        printf("key: %s%s\n", prefix, name);
        *rc = 0;
        return true;
    }
    return false;
}

static bool post_char(uint32_t cp)
{
    key_event_t ev = { .type = KEY_EV_CHAR, .cp = cp };
    return keyboard_post(&ev);
}

/* Decode one UTF-8 sequence; returns bytes consumed (0 on malformed input). */
static int utf8_next(const unsigned char *s, uint32_t *cp)
{
    if (s[0] < 0x80) { *cp = s[0]; return 1; }
    if ((s[0] & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) { *cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); return 2; }
    if ((s[0] & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); return 3;
    }
    if ((s[0] & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
        *cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); return 4;
    }
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    if (argc >= 3 && strcmp(argv[1], "text") == 0) {
        int sent = 0;
        for (int a = 2; a < argc; a++) {
            if (a > 2 && !post_char(' ')) return 1;
            for (const unsigned char *p = (const unsigned char *)argv[a]; *p;) {
                uint32_t cp;
                int n = utf8_next(p, &cp);
                if (!n) { printf("key: bad UTF-8\n"); return 1; }
                if (!post_char(cp)) { printf("key: queue full\n"); return 1; }
                p += n; sent++;
            }
        }
        printf("key: typed %d chars\n", sent);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "log") == 0 && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        keyboard_set_log(strcmp(argv[2], "on") == 0);
        printf("key: log %s\n", argv[2]);
        return 0;
    }
    /* ctrl + any one printable ASCII character, as given (key ctrl ], key ctrl p) */
    if (argc == 3 && strcmp(argv[1], "ctrl") == 0 && strlen(argv[2]) == 1 &&
        argv[2][0] >= 0x20 && argv[2][0] <= 0x7e) {
        key_event_t ev = { .type = KEY_EV_CHAR, .cp = (uint32_t)argv[2][0], .mods = MOD_CTRL };
        if (!keyboard_post(&ev)) { printf("key: queue full\n"); return 1; }
        printf("key: ctrl+%s\n", argv[2]);
        return 0;
    }
    int rc;
    if (argc == 3 && strcmp(argv[1], "ctrl") == 0 && post_named(argv[2], MOD_CTRL, "ctrl+", &rc)) return rc;
    if (argc == 3 && strcmp(argv[1], "shift") == 0 && post_named(argv[2], MOD_SHIFT, "shift+", &rc)) return rc;
    if (argc == 2 && post_named(argv[1], 0, "", &rc)) return rc;
    printf("usage: key <name> | key ctrl <char>|<name> | key shift <name> | key text <string> | key log on|off\n"
           "  names: up down left right enter esc tab backspace pgup pgdn home end\n");
    return 1;
}

/* `ui refresh` rebuilds the current screen over itself; `ui home` goes home the way a swipe
 * right at the bottom of the stack does; `ui settings` opens Settings as its footer button
 * does (or Ctrl+P on the dashboard). All run on the LVGL task. */
static void ui_refresh_async(void *arg) { (void)arg; ui_nav_refresh(); }
static void ui_home_async(void *arg) { (void)arg; ui_nav_home(); }
static void ui_settings_async(void *arg) { (void)arg; ui_nav_push(ui_settings_create, NULL); }
static int cmd_ui(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "refresh") == 0) { ui_async(ui_refresh_async, NULL); printf("ui: refresh\n"); return 0; }
    if (argc == 2 && strcmp(argv[1], "home") == 0) { ui_async(ui_home_async, NULL); printf("ui: home\n"); return 0; }
    if (argc == 2 && strcmp(argv[1], "settings") == 0) { ui_async(ui_settings_async, NULL); printf("ui: settings\n"); return 0; }
    printf("usage: ui refresh | ui home | ui settings\n");
    return 1;
}

void key_console_init(void)
{
    const esp_console_cmd_t ui_cmd = {
        .command = "ui",
        .help = "ui refresh: rebuild the current screen; ui home: go to the dashboard; ui settings: open Settings",
        .hint = NULL,
        .func = cmd_ui,
    };
    debug_console_register(&ui_cmd);
    const esp_console_cmd_t cmd = {
        .command = "key",
        .help = "Inject a key as if typed on the keyboard: <name>, ctrl <char>|<name>, shift <name>, text <string>; log on|off prints every decoded key",
        .hint = NULL,
        .func = cmd_key,
    };
    debug_console_register(&cmd);
}
