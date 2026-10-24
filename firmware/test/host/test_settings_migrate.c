#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "settings/settings_migrate.h"
#include "ui/ui_tokens.h"

void setUp(void) {}
void tearDown(void) {}

/* The v20 layout ends where v21 appended mac_lan_ip; its sizeof rounds up to the struct's alignment. */
#define V20_END   offsetof(app_settings_t, mac_lan_ip)
#define V20_SIZE  ((V20_END + 3u) & ~3u)

static uint8_t s_blob[sizeof(cfg_blob_t)];

/* Every byte a pattern (so "unchanged" means something), with ts_enable on and a known string or two. */
static app_settings_t patterned(void)
{
    app_settings_t s;
    uint8_t *p = (uint8_t *)&s;
    for (size_t i = 0; i < sizeof(s); i++) p[i] = (uint8_t)(i * 7 + 3);
    s.ts_enable = 1;
    strcpy(s.companion_server, "my-host");
    return s;
}

static size_t make_blob(uint32_t version, const app_settings_t *s, size_t body)
{
    memset(s_blob, 0, sizeof(s_blob));
    cfg_blob_t *b = (cfg_blob_t *)s_blob;
    b->magic = CFG_MAGIC;
    b->version = version;
    memcpy(&b->s, s, body);
    return 2 * sizeof(uint32_t) + body;
}

static void defaults(app_settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->ts_enable = 0;
}

/* The fields the v23 step writes (spec §10 steps 1, 2 and 4) */
static void assert_v23_step(const app_settings_t *out)
{
    TEST_ASSERT_EQUAL_HEX32(UI_TK_TERM_FG, out->term_fg);
    TEST_ASSERT_EQUAL_HEX32(UI_TK_TERM_BG, out->term_bg);
    TEST_ASSERT_EQUAL_HEX32(UI_TK_TERM_CURSOR, out->term_cursor);
    TEST_ASSERT_EQUAL_HEX32_ARRAY(ui_tk_term_ansi, out->term_ansi, 16);
    TEST_ASSERT_EQUAL_UINT8(0, out->term_cursor_style);
    TEST_ASSERT_EQUAL_UINT8(0, out->term_cursor_blink);
    TEST_ASSERT_EQUAL_UINT8(32, out->term_line_height);
    TEST_ASSERT_EQUAL_UINT8(1, out->rotation);
}

/* Puts back what the v23 step may write, from src, so a byte compare covers every other field. */
static void undo_v23_step(app_settings_t *out, const app_settings_t *src)
{
    out->term_fg = src->term_fg;
    out->term_bg = src->term_bg;
    out->term_cursor = src->term_cursor;
    memcpy(out->term_ansi, src->term_ansi, sizeof(out->term_ansi));
    out->term_cursor_style = src->term_cursor_style;
    out->term_cursor_blink = src->term_cursor_blink;
    out->term_line_height = src->term_line_height;
    out->rotation = src->rotation;
    memcpy(out->kbd_led_rgb, src->kbd_led_rgb, sizeof(out->kbd_led_rgb));
}

/* An old-style blob: the user's amber terminal, blinking, 26 px, portrait rotation. */
static app_settings_t old_look(void)
{
    app_settings_t s = patterned();
    s.term_fg = 0xFF9800;
    s.term_bg = 0x000000;
    s.term_cursor = 0xFF9800;
    for (int i = 0; i < 16; i++) s.term_ansi[i] = 0x111111u * (uint32_t)i;
    s.term_cursor_style = 2;
    s.term_cursor_blink = 1;
    s.term_line_height = 26;
    s.rotation = 0;
    s.kbd_led_rgb[0] = 0x10; s.kbd_led_rgb[1] = 0x20; s.kbd_led_rgb[2] = 0x30;   /* not amber: kept */
    memset(s.mac_lan_ip, 0, sizeof(s.mac_lan_ip));
    memset(s.mac_ts_ip, 0, sizeof(s.mac_ts_ip));
    strcpy(s.mac_lan_ip, "192.168.1.50");
    strcpy(s.mac_ts_ip, "100.101.102.103");
    return s;
}

static void test_settings_migrate_v20_turns_tailscale_off(void)
{
    app_settings_t src = patterned();
    size_t len = make_blob(20, &src, V20_SIZE);   /* the v20 tail padding lands on mac_lan_ip[0..] */
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(20, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(0, out.ts_enable);
    TEST_ASSERT_EQUAL_STRING("", out.mac_lan_ip);
    TEST_ASSERT_EQUAL_STRING("", out.mac_ts_ip);
    /* every other field of the v20 layout unchanged, once the v23 step's fields are put back */
    undo_v23_step(&out, &src);
    size_t ts = offsetof(app_settings_t, ts_enable);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, ts);
    TEST_ASSERT_EQUAL_MEMORY((uint8_t *)&src + ts + 1, (uint8_t *)&out + ts + 1, V20_END - ts - 1);
    TEST_ASSERT_EQUAL_STRING("my-host", out.companion_server);
    /* saved again it is a v23 blob */
    cfg_blob_t saved = settings_blob_make(&out);
    TEST_ASSERT_EQUAL_UINT32(CFG_MAGIC, saved.magic);
    TEST_ASSERT_EQUAL_UINT32(23, saved.version);
    TEST_ASSERT_EQUAL_UINT32(23, CFG_VERSION);
}

/* Older blobs get the same one-time switch-off. */
static void test_settings_migrate_older_turns_tailscale_off(void)
{
    app_settings_t src = patterned();
    size_t len = make_blob(17, &src, offsetof(app_settings_t, tz_rule));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(17, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(0, out.ts_enable);
    TEST_ASSERT_EQUAL_STRING("", out.mac_lan_ip);
}

/* Sticky: a current blob keeps the user's choice, on or off, and its addresses; the migration never runs again. */
static void test_settings_v23_keeps_tailscale_choice(void)
{
    app_settings_t src = patterned();
    memset(src.mac_lan_ip, 0, sizeof(src.mac_lan_ip));
    memset(src.mac_ts_ip, 0, sizeof(src.mac_ts_ip));
    strcpy(src.mac_lan_ip, "192.168.1.50");
    strcpy(src.mac_ts_ip, "100.101.102.103");
    size_t len = make_blob(23, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(23, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(1, out.ts_enable);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", out.mac_lan_ip);
    TEST_ASSERT_EQUAL_STRING("100.101.102.103", out.mac_ts_ip);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));

    src.ts_enable = 0;
    len = make_blob(23, &src, sizeof(src));
    defaults(&out);
    out.ts_enable = 1;   /* whatever the defaults say, the saved off stands */
    TEST_ASSERT_EQUAL_UINT32(23, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(0, out.ts_enable);
}

/* A blob that isn't ours leaves the defaults alone. */
static void test_settings_foreign_blob_keeps_defaults(void)
{
    app_settings_t src = patterned();
    size_t len = make_blob(23, &src, sizeof(src));
    ((cfg_blob_t *)s_blob)->magic = 0x12345678;
    app_settings_t out, want;
    defaults(&out);
    defaults(&want);
    TEST_ASSERT_EQUAL_UINT32(0, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_MEMORY(&want, &out, sizeof(out));
    TEST_ASSERT_EQUAL_UINT32(0, settings_blob_load(&out, s_blob, 4));   /* too short to hold a header */
    TEST_ASSERT_EQUAL_UINT32(0, settings_blob_load(&out, NULL, 0));
}

/* Strings from any blob end in a NUL. */
static void test_settings_addresses_terminated(void)
{
    app_settings_t src = patterned();
    memset(src.mac_lan_ip, '9', sizeof(src.mac_lan_ip));
    memset(src.mac_ts_ip, '9', sizeof(src.mac_ts_ip));
    size_t len = make_blob(21, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    settings_blob_load(&out, s_blob, len);
    TEST_ASSERT_EQUAL_CHAR(0, out.mac_lan_ip[sizeof(out.mac_lan_ip) - 1]);
    TEST_ASSERT_EQUAL_CHAR(0, out.mac_ts_ip[sizeof(out.mac_ts_ip) - 1]);
}

/* ---- CFG 23 (spec §10) ---- */
static void test_migrate_v21_to_v23_terminal_and_rotation(void)
{
    app_settings_t src = old_look();
    size_t len = make_blob(21, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(21, settings_blob_load(&out, s_blob, len));
    assert_v23_step(&out);
    TEST_ASSERT_EQUAL_STRING("my-host", out.companion_server);
    TEST_ASSERT_EQUAL_UINT8(1, out.ts_enable);   /* the v21 step doesn't run again */
    undo_v23_step(&out, &src);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));   /* nothing else moved */
}

static void test_migrate_v20_and_v22_to_v23(void)
{
    app_settings_t src = old_look();
    size_t len = make_blob(20, &src, V20_SIZE);
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(20, settings_blob_load(&out, s_blob, len));
    assert_v23_step(&out);
    TEST_ASSERT_EQUAL_UINT8(0, out.ts_enable);   /* the v21 step still runs from v20 */
    TEST_ASSERT_EQUAL_STRING("", out.mac_lan_ip);
    TEST_ASSERT_EQUAL_STRING("my-host", out.companion_server);

    len = make_blob(22, &src, sizeof(src));   /* Plan 2 Task 10's reserved number, same layout */
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(22, settings_blob_load(&out, s_blob, len));
    assert_v23_step(&out);
    TEST_ASSERT_EQUAL_UINT8(1, out.ts_enable);
    TEST_ASSERT_EQUAL_STRING("my-host", out.companion_server);
    undo_v23_step(&out, &src);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));
}

static void test_v23_blob_unchanged(void)
{
    app_settings_t src = old_look();
    src.term_line_height = 30;
    src.kbd_led_rgb[0] = 0xf4; src.kbd_led_rgb[1] = 0xb5; src.kbd_led_rgb[2] = 0x45;   /* settings.c's rule, not this one */
    size_t len = make_blob(23, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(23, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(30, out.term_line_height);
    TEST_ASSERT_EQUAL_UINT8(1, out.term_cursor_blink);
    TEST_ASSERT_EQUAL_UINT8(0, out.rotation);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));
}

static void test_migrate_amber_kbd_to_blue(void)
{
    app_settings_t src = old_look();
    src.kbd_led_rgb[0] = 0xf4; src.kbd_led_rgb[1] = 0xb5; src.kbd_led_rgb[2] = 0x45;
    size_t len = make_blob(21, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(21, settings_blob_load(&out, s_blob, len));
    const uint8_t blue[3] = { 0x00, 0x80, 0xff };
    TEST_ASSERT_EQUAL_HEX8_ARRAY(blue, out.kbd_led_rgb, 3);

    /* a colour that isn't amber-like stays */
    src.kbd_led_rgb[0] = 0x10; src.kbd_led_rgb[1] = 0x20; src.kbd_led_rgb[2] = 0x30;
    len = make_blob(21, &src, sizeof(src));
    defaults(&out);
    settings_blob_load(&out, s_blob, len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(src.kbd_led_rgb, out.kbd_led_rgb, 3);
}

/* A blob from a newer build (24): copy what fits and return 24; no older step runs (the downgrade path). */
static void test_newer_blob_takes_newer_path(void)
{
    app_settings_t src = old_look();
    size_t len = make_blob(24, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(24, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));
    TEST_ASSERT_EQUAL_UINT8(1, out.ts_enable);

    /* a longer newer blob: only what fits is copied */
    uint8_t big[sizeof(cfg_blob_t) + 16];
    memset(big, 0x5a, sizeof(big));
    memcpy(big, s_blob, len);
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(24, settings_blob_load(&out, big, sizeof(big)));
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));
}

/* ---- CFG 23 step 3, the TF-card hosts (I1, M2, M3: settings_sd_import_step) ---- */
static void test_sd_import_step_flag_off(void)
{
    for (int checkin = 0; checkin <= 1; checkin++)
        for (int r = -1; r <= 3; r++) {
            sd_import_plan_t p = settings_sd_import_step(0, checkin, r);
            TEST_ASSERT_FALSE(p.attempt);
            TEST_ASSERT_FALSE(p.clear);
        }
    sd_import_plan_t p = settings_sd_import_step(2, false, 1);   /* only 1 means "on the card" */
    TEST_ASSERT_FALSE(p.attempt);
}

static void test_sd_import_step_full_boot(void)
{
    sd_import_plan_t p = settings_sd_import_step(1, false, -1);
    TEST_ASSERT_TRUE(p.attempt);
    TEST_ASSERT_FALSE(p.clear);    /* no card or a read error: keep the flag, try again next boot (T11-13) */
    p = settings_sd_import_step(1, false, 0);
    TEST_ASSERT_TRUE(p.attempt);
    TEST_ASSERT_TRUE(p.clear);     /* read, nothing new */
    p = settings_sd_import_step(1, false, 3);
    TEST_ASSERT_TRUE(p.attempt);
    TEST_ASSERT_TRUE(p.clear);
}

static void test_sd_import_step_checkin_boot(void)
{
    for (int r = -1; r <= 3; r++) {
        sd_import_plan_t p = settings_sd_import_step(1, true, r);   /* a timer check-in never mounts */
        TEST_ASSERT_FALSE(p.attempt);
        TEST_ASSERT_FALSE(p.clear);
    }
}

/* I1: the step keys on the flag, not the version: a v23 blob still holding creds_on_sd 1 (a reset between the v23
   save and the import) loads it unchanged, and the step then attempts */
static void test_v23_blob_keeps_creds_on_sd_for_retry(void)
{
    app_settings_t src = old_look();
    src.creds_on_sd = 1;
    size_t len = make_blob(23, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(23, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(1, out.creds_on_sd);
    TEST_ASSERT_TRUE(settings_sd_import_step(out.creds_on_sd, false, -1).attempt);
}

/* ---- Reset settings (spec §5.12, U3): power, keyboard light and display brightness; the rest stays ---- */
static app_settings_t reset_defaults(void)
{
    app_settings_t d;
    uint8_t *p = (uint8_t *)&d;
    for (size_t i = 0; i < sizeof(d); i++) p[i] = (uint8_t)(i * 13 + 101);   /* differs from patterned() */
    d.pw_dim_s = 30;
    d.kbd_led_bright = 20;
    d.brightness = 80;
    return d;
}

static void test_reset_scope_resets_power_light_brightness(void)
{
    app_settings_t s = patterned(), d = reset_defaults();
    s.pw_dim_s = 600;
    s.kbd_led_bright = 90;
    s.brightness = 30;
    s.pw_wake_lift = (uint8_t)(d.pw_wake_lift + 1);
    s.pw_light_sleep = (uint8_t)(d.pw_light_sleep + 1);
    s.pw_hibernate_checkin_h = (uint8_t)(d.pw_hibernate_checkin_h + 1);
    settings_reset_scope(&s, &d);
    TEST_ASSERT_EQUAL_UINT16(d.pw_dim_s, s.pw_dim_s);
    TEST_ASSERT_EQUAL_UINT8(d.kbd_led_bright, s.kbd_led_bright);
    TEST_ASSERT_EQUAL_UINT8(d.brightness, s.brightness);
    TEST_ASSERT_EQUAL_UINT16(d.pw_off_s, s.pw_off_s);
    TEST_ASSERT_EQUAL_UINT8(d.pw_sleep_on_usb, s.pw_sleep_on_usb);
    TEST_ASSERT_EQUAL_UINT8(d.pw_wake_needs_you, s.pw_wake_needs_you);
    TEST_ASSERT_EQUAL_UINT8(d.pw_radio_off, s.pw_radio_off);
    TEST_ASSERT_EQUAL_UINT16(d.pw_radio_off_min, s.pw_radio_off_min);
    TEST_ASSERT_EQUAL_UINT16(d.pw_checkin_min, s.pw_checkin_min);
    TEST_ASSERT_EQUAL_UINT16(d.pw_deep_idle_min, s.pw_deep_idle_min);
    TEST_ASSERT_EQUAL_UINT8(d.pw_hibernate_h, s.pw_hibernate_h);
    TEST_ASSERT_EQUAL_UINT16(d.pw_hibernate_checkin_min, s.pw_hibernate_checkin_min);
    TEST_ASSERT_EQUAL_UINT8(d.pw_wake_lift, s.pw_wake_lift);                     /* reserved, still pw_* */
    TEST_ASSERT_EQUAL_UINT8(d.pw_light_sleep, s.pw_light_sleep);
    TEST_ASSERT_EQUAL_UINT8(d.pw_hibernate_checkin_h, s.pw_hibernate_checkin_h); /* reserved, still pw_* */
    TEST_ASSERT_EQUAL_UINT8(d.ss_disable_in_ssh, s.ss_disable_in_ssh);
    TEST_ASSERT_EQUAL_UINT8(d.kbd_led_mode, s.kbd_led_mode);
    TEST_ASSERT_EQUAL_MEMORY(d.kbd_led_rgb, s.kbd_led_rgb, 3);
    TEST_ASSERT_EQUAL_UINT8(d.kbd_needs_light, s.kbd_needs_light);
    TEST_ASSERT_EQUAL_UINT8(d.kbd_dark_light, s.kbd_dark_light);
}

static void test_reset_scope_keeps_identity_and_tz(void)
{
    app_settings_t s = patterned(), d = reset_defaults();
    strcpy(s.claude_path, "~/bin/claude");
    strcpy(s.mac_lan_ip, "192.168.1.50");
    strcpy(s.ts_auth_key, "tskey-auth-preview");
    strcpy(s.ts_device_name, "my-tab5");
    strcpy(s.tz_rule, "JST-9");
    strcpy(s.ota_url, "https://example.com/m.json");
    s.kbd_layout = 1;
    s.volume = 33;
    const app_settings_t before = s;
    settings_reset_scope(&s, &d);
    TEST_ASSERT_EQUAL_STRING(before.companion_server, s.companion_server);
    TEST_ASSERT_EQUAL_STRING("~/bin/claude", s.claude_path);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", s.mac_lan_ip);
    TEST_ASSERT_EQUAL_UINT8(before.ts_enable, s.ts_enable);
    TEST_ASSERT_EQUAL_STRING("tskey-auth-preview", s.ts_auth_key);
    TEST_ASSERT_EQUAL_STRING("my-tab5", s.ts_device_name);
    TEST_ASSERT_EQUAL_STRING("JST-9", s.tz_rule);
    TEST_ASSERT_EQUAL_UINT8(1, s.kbd_layout);
    TEST_ASSERT_EQUAL_UINT8(33, s.volume);
    TEST_ASSERT_EQUAL_STRING("https://example.com/m.json", s.ota_url);
    /* and every byte outside the reset fields: put the reset fields back and the struct is unchanged */
    s.brightness = before.brightness;
    s.ss_disable_in_ssh = before.ss_disable_in_ssh;
    s.kbd_led_mode = before.kbd_led_mode;
    s.kbd_led_bright = before.kbd_led_bright;
    memcpy(s.kbd_led_rgb, before.kbd_led_rgb, 3);
    size_t a = offsetof(app_settings_t, pw_dim_s), b = offsetof(app_settings_t, mac_lan_ip);
    memcpy((uint8_t *)&s + a, (const uint8_t *)&before + a, b - a);   /* pw_* .. pw_hibernate_checkin_min, the light pair */
    TEST_ASSERT_EQUAL_MEMORY(&before, &s, sizeof(s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_settings_migrate_v20_turns_tailscale_off);
    RUN_TEST(test_settings_migrate_older_turns_tailscale_off);
    RUN_TEST(test_settings_v23_keeps_tailscale_choice);
    RUN_TEST(test_settings_foreign_blob_keeps_defaults);
    RUN_TEST(test_settings_addresses_terminated);
    RUN_TEST(test_migrate_v21_to_v23_terminal_and_rotation);
    RUN_TEST(test_migrate_v20_and_v22_to_v23);
    RUN_TEST(test_v23_blob_unchanged);
    RUN_TEST(test_migrate_amber_kbd_to_blue);
    RUN_TEST(test_newer_blob_takes_newer_path);
    RUN_TEST(test_sd_import_step_flag_off);
    RUN_TEST(test_sd_import_step_full_boot);
    RUN_TEST(test_sd_import_step_checkin_boot);
    RUN_TEST(test_v23_blob_keeps_creds_on_sd_for_retry);
    RUN_TEST(test_reset_scope_resets_power_light_brightness);
    RUN_TEST(test_reset_scope_keeps_identity_and_tz);
    return UNITY_END();
}
