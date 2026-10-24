#include <string.h>
#include "unity.h"
#include "sys/url_mask.h"

void setUp(void) {}
void tearDown(void) {}

static const char *mask(const char *u)
{
    static char b[200];
    url_mask(u, b, sizeof(b));
    return b;
}

static void test_plain_url_prints_whole(void)
{
    TEST_ASSERT_EQUAL_STRING("https://updates.example.com/m.json", mask("https://updates.example.com/m.json"));
    TEST_ASSERT_EQUAL_STRING("none", mask(""));
}

static void test_query_is_masked(void)
{
    TEST_ASSERT_EQUAL_STRING("https://updates.example.com/\xe2\x80\xa6?<masked>",
                             mask("https://updates.example.com/tab5/manifest.json?token=secret123"));
}

static void test_userinfo_is_masked(void)
{
    const char *m = mask("https://user:tok@host.example/m.json");
    TEST_ASSERT_NULL(strstr(m, "tok"));
    TEST_ASSERT_NULL(strstr(m, "user"));
    TEST_ASSERT_EQUAL_STRING("https://<masked>@host.example/m.json", m);
    m = mask("https://user:tok@host.example/m?x=1");
    TEST_ASSERT_NULL(strstr(m, "tok"));
    TEST_ASSERT_EQUAL_STRING("https://<masked>@host.example/\xe2\x80\xa6?<masked>", m);
}

static void test_at_in_path_is_not_userinfo(void)
{
    TEST_ASSERT_EQUAL_STRING("https://host.example/a@b", mask("https://host.example/a@b"));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_plain_url_prints_whole);
    RUN_TEST(test_query_is_masked);
    RUN_TEST(test_userinfo_is_masked);
    RUN_TEST(test_at_in_path_is_not_userinfo);
    return UNITY_END();
}
