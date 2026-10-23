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
};

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
    if (argc == 3 && strcmp(argv[1], "ctrl") == 0 && strlen(argv[2]) == 1 &&
        argv[2][0] >= 'a' && argv[2][0] <= 'z') {
        key_event_t ev = { .type = KEY_EV_CHAR, .cp = (uint32_t)argv[2][0], .mods = MOD_CTRL };
        if (!keyboard_post(&ev)) { printf("key: queue full\n"); return 1; }
        printf("key: ctrl+%s\n", argv[2]);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "ctrl") == 0) {   /* ctrl + a named key, e.g. ctrl tab */
        for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); i++) {
            if (strcmp(argv[2], s_names[i].name) != 0) continue;
            key_event_t ev = { .type = KEY_EV_SPECIAL, .key = s_names[i].key, .mods = MOD_CTRL };
            if (!keyboard_post(&ev)) { printf("key: queue full\n"); return 1; }
            printf("key: ctrl+%s\n", argv[2]);
            return 0;
        }
    }
    if (argc == 2) {
        for (size_t i = 0; i < sizeof(s_names) / sizeof(s_names[0]); i++) {
            if (strcmp(argv[1], s_names[i].name) != 0) continue;
            key_event_t ev = { .type = KEY_EV_SPECIAL, .key = s_names[i].key };
            if (!keyboard_post(&ev)) { printf("key: queue full\n"); return 1; }
            printf("key: %s\n", argv[1]);
            return 0;
        }
    }
    printf("usage: key up|down|left|right|enter|esc|tab|backspace | key ctrl <a-z>|<key> | key text <string>\n");
    return 1;
}

/* `ui refresh` rebuilds the current screen over itself; `ui home` goes home the way a swipe
 * right at the bottom of the stack does; `ui settings` opens Settings as its footer button
 * does (it has no key). All run on the LVGL task. */
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
        .help = "Inject a key as if typed on the keyboard: up|down|left|right|enter|esc|tab|backspace, ctrl <a-z>, or text <string>",
        .hint = NULL,
        .func = cmd_key,
    };
    debug_console_register(&cmd);
}
