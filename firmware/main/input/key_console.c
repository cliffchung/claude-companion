#include "keyboard.h"
#include "sys/debug_console.h"
#include "esp_console.h"
#include <stdio.h>
#include <string.h>
#include "ui/ui.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "settings/settings.h"
#include "sys/power.h"
#include "sys/power_rules.h"
#include "bsp/m5stack_tab5.h"
#include <stdlib.h>
#include "src/misc/lv_timer_private.h" /* LVGL 9.6 has no public period/last-run accessor. */

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

/* The whole of s as a number in base: trailing junk ("1x") or an empty string is refused. */
static bool parse_long(const char *s, int base, long *out)
{
    char *end;
    if (!s || !*s || *s == ' ') return false;
    long v = strtol(s, &end, base);
    if (*end) return false;
    *out = v;
    return true;
}

/* `key layout [stock|jis]`: the keyboard layout (it left Settings in Plan 4). No argument prints it. */
static int key_layout(int argc, char **argv)
{
    if (argc == 3) {
        uint8_t l = strcmp(argv[2], "stock") == 0 ? KBD_LAYOUT_STOCK : strcmp(argv[2], "jis") == 0 ? KBD_LAYOUT_JIS : 0xFF;
        if (l == 0xFF) { printf("usage: key layout [stock|jis]\n"); return 1; }
        if (l != g_settings.kbd_layout) {
            g_settings.kbd_layout = l;
            settings_save();
            keyboard_set_layout(l);
        }
    }
    printf("key layout: %s\n", g_settings.kbd_layout == KBD_LAYOUT_JIS ? "jis" : "stock");
    return 0;
}

/* `key light [<mode 0|1> <bright 0..100> <rrggbb> [<needs 0|1> <dark 2..100>]]`: the saved keyboard light, as
   Settings › Keyboard light sets it; no argument prints it (a test records it here and puts it back). */
static int key_light(int argc, char **argv)
{
    if (argc == 5 || argc == 7) {
        long mode, bright, rgb, needs = 1, dark = g_settings.kbd_dark_light;
        bool ok = parse_long(argv[2], 10, &mode) && parse_long(argv[3], 10, &bright) && strlen(argv[4]) == 6 &&
                  parse_long(argv[4], 16, &rgb);
        if (argc == 7) ok = ok && parse_long(argv[5], 10, &needs) && parse_long(argv[6], 10, &dark);
        if (!ok || mode < 0 || mode > 1 || bright < 0 || bright > 100 || rgb < 0 || needs < 0 || needs > 1 ||
            dark < 2 || dark > 100) {
            printf("usage: key light [<mode 0|1> <bright 0..100> <rrggbb> [<needs 0|1> <dark 2..100>]]\n");
            return 1;
        }
        if (pr_rgb_amber_like((uint32_t)rgb)) { printf("key light: amber means needs-you only\n"); return 1; }
        g_settings.kbd_led_mode = (uint8_t)mode;
        g_settings.kbd_led_bright = (uint8_t)bright;
        g_settings.kbd_led_rgb[0] = (uint8_t)(rgb >> 16);
        g_settings.kbd_led_rgb[1] = (uint8_t)(rgb >> 8);
        g_settings.kbd_led_rgb[2] = (uint8_t)rgb;
        if (argc == 7) { g_settings.kbd_needs_light = (uint8_t)needs; g_settings.kbd_dark_light = (uint8_t)dark; }
        settings_save();
        power_kbd_light_refresh();
    } else if (argc != 2) {
        printf("usage: key light [<mode 0|1> <bright 0..100> <rrggbb> [<needs 0|1> <dark 2..100>]]\n");
        return 1;
    }
    printf("key light: mode=%u bright=%u rgb=%02x%02x%02x needs=%u dark=%u\n", g_settings.kbd_led_mode,
           g_settings.kbd_led_bright, g_settings.kbd_led_rgb[0], g_settings.kbd_led_rgb[1], g_settings.kbd_led_rgb[2],
           g_settings.kbd_needs_light, g_settings.kbd_dark_light);
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "layout") == 0 && argc <= 3) return key_layout(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "light") == 0) return key_light(argc, argv);
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
        printf("key: log %s%s\n", argv[2], strcmp(argv[2], "on") == 0 ? " (typed text masked; off by itself after 5 min)" : "");
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
    if (argc == 3 && strcmp(argv[1], "ctrl") == 0 && strcmp(argv[2], "space") == 0) {
        key_event_t ev = { .type = KEY_EV_CHAR, .cp = ' ', .mods = MOD_CTRL };
        if (!keyboard_post(&ev)) { printf("key: queue full\n"); return 1; }
        printf("key: ctrl+space\n");
        return 0;
    }
    int rc;
    if (argc == 3 && strcmp(argv[1], "ctrl") == 0 && post_named(argv[2], MOD_CTRL, "ctrl+", &rc)) return rc;
    if (argc == 3 && strcmp(argv[1], "shift") == 0 && post_named(argv[2], MOD_SHIFT, "shift+", &rc)) return rc;
    if (argc == 2 && post_named(argv[1], 0, "", &rc)) return rc;
    printf("usage: key <name> | key ctrl <char>|<name>|space | key shift <name> | key text <string> | key log on|off | "
           "key layout [stock|jis] | key light [<mode> <bright> <rrggbb> [<needs> <dark>]]\n"
           "  names: up down left right enter esc tab backspace pgup pgdn home end\n");
    return 1;
}

/* `ui refresh` rebuilds the current screen over itself; `ui home` goes home the way a swipe
 * right at the bottom of the stack does; `ui settings` opens Settings as its footer button
 * does (or Ctrl+P on the dashboard). All run on the LVGL task. */
static void ui_refresh_async(void *arg) { (void)arg; ui_nav_refresh(); }
static void ui_home_async(void *arg) { (void)arg; ui_nav_home(); }
static void ui_settings_async(void *arg) { (void)arg; ui_nav_push(ui_settings_create, NULL); }
/* `ui heapmin start|stop`: the internal-RAM low-water mark over an action (the B2 RAM gate).
 * Reads heap counters only, so it takes no LVGL lock. */
static int ui_heapmin(const char *arg)
{
    if (strcmp(arg, "start") == 0) {
        if (heap_caps_monitor_local_minimum_free_size_start() != ESP_OK) {
            printf("heapmin: already started\n");
            return 1;
        }
        printf("heapmin: started\n");
        return 0;
    }
    if (strcmp(arg, "stop") == 0) {
        size_t min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        if (heap_caps_monitor_local_minimum_free_size_stop() != ESP_OK) {
            printf("heapmin: not started\n");
            return 1;
        }
        printf("heapmin: internal min %u B, largest %u B, now %u B\n", (unsigned)min,
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return 0;
    }
    printf("usage: ui heapmin start|stop\n");
    return 1;
}

/* `ui timers`: every LVGL timer with its period, so a gate can count the fast ones (B1). */
static int ui_timers(void)
{
    ui_lock();
    uint32_t now = lv_tick_get();
    int n = 0, fast = 0;
    for (lv_timer_t *t = lv_timer_get_next(NULL); t; t = lv_timer_get_next(t)) {
        printf("timer %u ms, last run %u ms ago, paused=%d\n", (unsigned)t->period,
               (unsigned)lv_tick_diff(now, t->last_run), t->paused ? 1 : 0);
        n++;
        if (t->period < 300) fast++;
    }
    ui_unlock();
    printf("timers: %d, under 300 ms: %d\n", n, fast);
    return 0;
}

static int cmd_ui(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "heapmin") == 0) return ui_heapmin(argv[2]);
    if (argc == 2 && strcmp(argv[1], "timers") == 0) return ui_timers();
    if (argc == 2 && strcmp(argv[1], "refresh") == 0) { ui_async(ui_refresh_async, NULL); printf("ui: refresh\n"); return 0; }
    if (argc == 2 && strcmp(argv[1], "home") == 0) { ui_async(ui_home_async, NULL); printf("ui: home\n"); return 0; }
    if (argc == 2 && strcmp(argv[1], "settings") == 0) { ui_async(ui_settings_async, NULL); printf("ui: settings\n"); return 0; }
    if (argc >= 2 && argc <= 3 && strcmp(argv[1], "brightness") == 0) {
        if (argc == 3) {
            long v;
            if (!parse_long(argv[2], 10, &v) || v < 5 || v > 100) { printf("usage: ui brightness [5..100]\n"); return 1; }
            g_settings.brightness = (uint8_t)v;
            settings_save();
            if (power_state() == PS_ACTIVE) bsp_display_brightness_set(v);
        }
        printf("ui brightness: %u\n", g_settings.brightness);
        return 0;
    }
    printf("usage: ui refresh | ui home | ui settings | ui heapmin start|stop | ui timers | ui brightness [5..100]\n");
    return 1;
}

void key_console_init(void)
{
    const esp_console_cmd_t ui_cmd = {
        .command = "ui",
        .help = "ui refresh: rebuild the current screen; ui home: go to Sessions; ui settings: open Settings; ui heapmin start|stop: internal RAM low over an action; ui timers: list LVGL timers; ui brightness [n]: the saved display brightness",
        .hint = NULL,
        .func = cmd_ui,
    };
    debug_console_register(&ui_cmd);
    const esp_console_cmd_t cmd = {
        .command = "key",
        .help = "Inject a key as if typed on the keyboard: <name>, ctrl <char>|<name>|space, shift <name>, text <string>; log on|off prints every decoded key; layout [stock|jis]; light [<mode> <bright> <rrggbb> [<needs> <dark>]]",
        .hint = NULL,
        .func = cmd_key,
    };
    debug_console_register(&cmd);
}
