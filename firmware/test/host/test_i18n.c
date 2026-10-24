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
        while (*p && strchr("-+ #0123456789.", *p)) p++;
        while (*p && strchr("hlzjtL", *p)) p++;
        out[n++] = (*p && strchr("diouxXeEfFgGcsp", *p)) ? *p : '?';
        if (!*p) break;
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
};
/* plain text with a literal percent sign: shown with "%s", never used as a format */
static const str_id_t k_plain_percent[] = { STR_SET_DARK_LOWEST, STR_SET_DARK_LOW, STR_SET_DARK_MEDIUM, STR_SES_SLEEPS_AT };

static void test_formats_match_their_callers(void)
{
    for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) {
        char c[16];
        conversions(tr(k_formats[i].id), c, sizeof(c));
        TEST_ASSERT_EQUAL_STRING_MESSAGE(k_formats[i].conv, c, tr(k_formats[i].id));
    }
}

static void test_no_other_entry_has_a_conversion(void)
{
    for (int id = 0; id < STR_COUNT; id++) {
        bool listed = false;
        for (size_t i = 0; i < sizeof(k_formats) / sizeof(k_formats[0]); i++) listed |= k_formats[i].id == id;
        for (size_t i = 0; i < sizeof(k_plain_percent) / sizeof(k_plain_percent[0]); i++) listed |= k_plain_percent[i] == id;
        if (listed) continue;
        TEST_ASSERT_NULL_MESSAGE(strchr(tr((str_id_t)id), '%'), tr((str_id_t)id));
    }
}

static void test_out_of_range_is_empty(void)
{
    TEST_ASSERT_EQUAL_STRING("", tr(STR_COUNT));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_formats_match_their_callers);
    RUN_TEST(test_no_other_entry_has_a_conversion);
    RUN_TEST(test_out_of_range_is_empty);
    return UNITY_END();
}
