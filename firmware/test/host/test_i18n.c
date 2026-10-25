/* The tr() table's printf formats (Task 12, the checked-format ruling): a format entry is passed to snprintf
 * as a non-literal, so the compiler can't check it against its arguments. This pins each one's conversions to
 * what its callers pass, and every other entry to having no conversion at all (a stray "%s" in plain text
 * would read an argument that isn't there). */
#include <stdbool.h>
#include <string.h>
#include "unity.h"
#include "i18n/i18n.h"

void setUp(void) {}
void tearDown(void) {}

/* The conversion letters of fmt in order ("d.d s" style: one letter per conversion, "%%" skipped); "?" on a
 * '%' that doesn't start a conversion. */
static void conversions(const char *fmt, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *p = fmt; *p && n + 1 < cap; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0", *p)) p++;                    /* flags */
        while (*p >= '0' && *p <= '9') p++;                       /* width */
        if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }   /* precision */
        while (*p && strchr("hlzjtL", *p)) p++;                    /* length */
        if (*p && strchr("diouxXeEfFgGcsp", *p)) { out[n++] = *p; continue; }
        out[n++] = '?';
        if (!*p) break;
        p--;   /* the character that ended it is read again: it may start the next conversion */
    }
    out[n] = 0;
}

static const struct { str_id_t id; const char *conv; } k_formats[] = {
    { STR_DIAG_CHIP, "dds" },
    { STR_DIAG_INTERNAL, "uuu" },
    { STR_DIAG_PSRAM, "uu" },
    { STR_DIAG_BATTERY, "fdfs" },
    { STR_DIAG_WIFI, "dds" },
    { STR_DIAG_REST, "ssss" },
    { STR_NET_CONNECTED, "s" },
    { STR_NET_SAVED_N, "d" },
    { STR_NET_DISCONNECT_Q, "s" },
    { STR_NET_FORGET_Q, "s" },
    { STR_NET_FORGOTTEN, "s" },
    { STR_NET_PASSWORD_FOR, "s" },
    { STR_NET_JOINING, "s" },
    { STR_NET_JOINED, "s" },
    { STR_NET_STATIC_VALUE, "sds" },
    { STR_NET_TS_DERP, "s" },
    { STR_NET_TS_PEERS_ONLINE, "d" },
    { STR_NET_TS_N_ONLINE, "d" },
    { STR_NET_PEER_LINE, "sss" },
    { STR_TERM_CONNECTING_TO, "ssd" },
    { STR_TERM_DISCONNECT_Q, "s" },
    { STR_TERM_DISCONNECTED_FROM, "s" },
    { STR_TERM_SESSION_ENDED, "d" },
    { STR_HF_DELETE_Q, "s" },
    { STR_SET_TRUST_BODY, "s" },
    /* Plan 6 */
    { STR_SET_FW_AVAILABLE, "s" },
    { STR_SET_FW_AVAILABLE_OLDER, "s" },
    { STR_FW_CONFIRM_Q, "s" },
    { STR_FW_CONFIRM_BODY, "s" },
    { STR_FW_BYTES, "sd" },
    { STR_FW_UPDATED, "s" },
    { STR_FW_ROLLED_BACK, "ss" },
    { STR_FW_ROLLING_BACK, "s" },
    { STR_FW_BOOTLOOP, "s" },
    { STR_FW_GO_BACK_Q, "s" },
};
/* plain text with a literal percent sign: shown with "%s", never used as a format. Each one's conversion list is
   pinned too ("?" is its lone '%', which starts no conversion), so none can gain a stray "%s" (review M3). */
static const struct { str_id_t id; const char *conv; } k_plain_percent[] = {
    { STR_SET_DARK_LOWEST, "?" }, { STR_SET_DARK_LOW, "?" }, { STR_SET_DARK_MEDIUM, "?" }, { STR_SES_SLEEPS_AT, "?" },
    /* "30 % or" reads as a "% o" conversion: pinned, and only ever shown through "%s" or as a toast */
    { STR_FW_ERR_BATTERY, "o" },
};

static void test_formats_match_their_callers(void)
{
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
        char c[16];
        conversions(tr(k_formats[i].id), c, sizeof(c));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_formats[i].conv, c, tr(k_formats[i].id));
    }
}

static void test_plain_percent_entries_have_no_conversion(void)
{
    for (size_t i = 0; i < sizeof(k_plain_percent) / sizeof(k_plain_percent[0]); i++) {
        char c[16];
        conversions(tr(k_plain_percent[i].id), c, sizeof(c));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_plain_percent[i].conv, c, tr(k_plain_percent[i].id));
    }
}

static void test_no_other_entry_has_a_conversion(void)
{
    for (int id = 0; id < STR_COUNT; id++) {
        bool listed = false;
        for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) listed |= k_formats[i].id == id;
        for (size_t i = 0; i < sizeof(k_plain_percent) / sizeof(k_plain_percent[0]); i++) listed |= k_plain_percent[i].id == id;
        if (listed) continue;
        TEST_ASSERT_NULL_MESSAGE(strchr(tr((str_id_t)id), '%'), tr((str_id_t)id));
    }
}

static void test_out_of_range_is_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", tr(STR_COUNT));
}

/* ui_touch_text's mapping: each key hint and its touch wording, which names no key */
static const struct { str_id_t keys, touch; } k_touch[] = {
    { STR_HOSTS_LEAVE_NOTE, STR_HOSTS_LEAVE_NOTE_TOUCH },
    { STR_TERM_OPENING_HINT, STR_TERM_OPENING_HINT_TOUCH },
    { STR_TERM_SSH_HINT, STR_TERM_SSH_HINT_TOUCH },
    { STR_VOICE_HINT, STR_VOICE_HINT_TOUCH },
    { STR_VOICE_NOT_AT_PROMPT, STR_VOICE_NOT_AT_PROMPT_TOUCH },
    { STR_VOICE_NOT_READY, STR_VOICE_NOT_READY_TOUCH },
    { STR_VOICE_REVIEW, STR_VOICE_REVIEW_TOUCH },
    { STR_VOICE_TOO_SHORT, STR_VOICE_TOO_SHORT_TOUCH },
};

static void test_touch_text_maps_each_key_hint(void)
{
    for (size_t i = 0; i < sizeof(k_touch) / sizeof(k_touch[0]); i++) {
        const char *k = tr(k_touch[i].keys);
        TEST_ASSERT_EQUAL_MESSAGE(k_touch[i].keys, i18n_touch_text(k_touch[i].keys, true), k);
        TEST_ASSERT_EQUAL_MESSAGE(k_touch[i].touch, i18n_touch_text(k_touch[i].keys, false), k);
        const char *t = tr(k_touch[i].touch);
        TEST_ASSERT_NULL_MESSAGE(strstr(t, "Ctrl"), t);
        TEST_ASSERT_NULL_MESSAGE(strstr(t, "Press"), t);
        TEST_ASSERT_NULL_MESSAGE(strstr(t, "Esc"), t);
        TEST_ASSERT_NOT_NULL_MESSAGE(strstr(t, "Tap") ? strstr(t, "Tap") : strstr(t, "tap"), t);
    }
}

static void test_touch_text_leaves_other_ids(void)
{
    for (int id = 0; id < STR_COUNT; id++) {
        bool listed = false;
        for (size_t i = 0; i < sizeof(k_touch) / sizeof(k_touch[0]); i++) listed |= k_touch[i].keys == (str_id_t)id;
        if (listed) continue;
        TEST_ASSERT_EQUAL_MESSAGE(id, i18n_touch_text((str_id_t)id, false), tr((str_id_t)id));
        TEST_ASSERT_EQUAL_MESSAGE(id, i18n_touch_text((str_id_t)id, true), tr((str_id_t)id));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_formats_match_their_callers);
    RUN_TEST(test_plain_percent_entries_have_no_conversion);
    RUN_TEST(test_no_other_entry_has_a_conversion);
    RUN_TEST(test_out_of_range_is_empty);
    RUN_TEST(test_touch_text_maps_each_key_hint);
    RUN_TEST(test_touch_text_leaves_other_ids);
    return UNITY_END();
}
