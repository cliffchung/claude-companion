/* term_scroll: the bytes a take-over sends to scroll Claude Code (PgUp / PgDn, SGR wheel reports). */
#include <string.h>
#include "unity.h"
#include "term/term_scroll.h"

void setUp(void) {}
void tearDown(void) {}

static void test_page(void)
{
    char b[16];
    TEST_ASSERT_EQUAL(4, term_scroll_page(true, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("\x1b[5~", b);
    TEST_ASSERT_EQUAL(4, term_scroll_page(false, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING("\x1b[6~", b);
    TEST_ASSERT_EQUAL(0, term_scroll_page(true, b, 4));   /* no room for the NUL */
    TEST_ASSERT_EQUAL(0, term_scroll_page(true, NULL, 16));
}

static void test_wheel(void)
{
    char b[128];
    int k = -1;
    TEST_ASSERT_EQUAL(12, term_scroll_wheel(true, 1, 50, 20, b, sizeof(b), &k));
    TEST_ASSERT_EQUAL_STRING("\x1b[<64;50;20M", b);
    TEST_ASSERT_EQUAL(1, k);
    TEST_ASSERT_EQUAL(20, term_scroll_wheel(false, 2, 7, 3, b, sizeof(b), &k));
    TEST_ASSERT_EQUAL_STRING("\x1b[<65;7;3M\x1b[<65;7;3M", b);
    TEST_ASSERT_EQUAL(2, k);
    /* 1-based: a cell before the first counts as the first */
    term_scroll_wheel(true, 1, 0, -4, b, sizeof(b), &k);
    TEST_ASSERT_EQUAL_STRING("\x1b[<64;1;1M", b);
    /* only whole reports: 3 asked, 2 fit with the NUL */
    TEST_ASSERT_EQUAL(20, term_scroll_wheel(true, 3, 1, 1, b, 2 * 10 + 1, &k));
    TEST_ASSERT_EQUAL(2, k);
    TEST_ASSERT_EQUAL(20, (int)strlen(b));
    TEST_ASSERT_EQUAL(0, term_scroll_wheel(true, 0, 1, 1, b, sizeof(b), &k));
    TEST_ASSERT_EQUAL(0, k);
    TEST_ASSERT_EQUAL_STRING("", b);
    TEST_ASSERT_EQUAL(0, term_scroll_wheel(true, 1, 1, 1, b, 10, &k));   /* not even one fits (10 bytes and the NUL) */
    TEST_ASSERT_EQUAL(0, k);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_page);
    RUN_TEST(test_wheel);
    return UNITY_END();
}
