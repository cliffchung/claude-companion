#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "unity.h"
#include "settings/settings_migrate.h"

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
    /* every other field of the v20 layout unchanged */
    size_t ts = offsetof(app_settings_t, ts_enable);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, ts);
    TEST_ASSERT_EQUAL_MEMORY((uint8_t *)&src + ts + 1, (uint8_t *)&out + ts + 1, V20_END - ts - 1);
    TEST_ASSERT_EQUAL_STRING("my-host", out.companion_server);
    /* saved again it is a v21 blob */
    cfg_blob_t saved = settings_blob_make(&out);
    TEST_ASSERT_EQUAL_UINT32(CFG_MAGIC, saved.magic);
    TEST_ASSERT_EQUAL_UINT32(21, saved.version);
    TEST_ASSERT_EQUAL_UINT32(21, CFG_VERSION);
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

/* Sticky: a v21 blob keeps the user's choice, on or off, and its addresses; the migration never runs again. */
static void test_settings_v21_keeps_tailscale_choice(void)
{
    app_settings_t src = patterned();
    memset(src.mac_lan_ip, 0, sizeof(src.mac_lan_ip));
    memset(src.mac_ts_ip, 0, sizeof(src.mac_ts_ip));
    strcpy(src.mac_lan_ip, "192.168.1.50");
    strcpy(src.mac_ts_ip, "100.101.102.103");
    size_t len = make_blob(21, &src, sizeof(src));
    app_settings_t out;
    defaults(&out);
    TEST_ASSERT_EQUAL_UINT32(21, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(1, out.ts_enable);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", out.mac_lan_ip);
    TEST_ASSERT_EQUAL_STRING("100.101.102.103", out.mac_ts_ip);
    TEST_ASSERT_EQUAL_MEMORY(&src, &out, sizeof(src));

    src.ts_enable = 0;
    len = make_blob(21, &src, sizeof(src));
    defaults(&out);
    out.ts_enable = 1;   /* whatever the defaults say, the saved off stands */
    TEST_ASSERT_EQUAL_UINT32(21, settings_blob_load(&out, s_blob, len));
    TEST_ASSERT_EQUAL_UINT8(0, out.ts_enable);
}

/* A blob that isn't ours leaves the defaults alone. */
static void test_settings_foreign_blob_keeps_defaults(void)
{
    app_settings_t src = patterned();
    size_t len = make_blob(21, &src, sizeof(src));
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_settings_migrate_v20_turns_tailscale_off);
    RUN_TEST(test_settings_migrate_older_turns_tailscale_off);
    RUN_TEST(test_settings_v21_keeps_tailscale_choice);
    RUN_TEST(test_settings_foreign_blob_keeps_defaults);
    RUN_TEST(test_settings_addresses_terminated);
    return UNITY_END();
}
