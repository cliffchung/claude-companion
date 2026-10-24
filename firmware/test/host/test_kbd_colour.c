#include <stdint.h>
#include "unity.h"
#include "settings/kbd_colour.h"
#include "sys/power_rules.h"

void setUp(void) {}
void tearDown(void) {}

static void test_swatches_never_amber(void)
{
    int n = 0;
    for (const kbd_swatch_t *s = kbd_swatches; s->label; s++, n++)
        TEST_ASSERT_FALSE_MESSAGE(pr_rgb_amber_like(s->rgb), s->label);
    TEST_ASSERT_EQUAL_INT(6, n);
    TEST_ASSERT_EQUAL_HEX32(0x0080FF, kbd_swatches[0].rgb);   /* Blue, the default, first */
}

static void test_index_round_trip(void)
{
    for (int i = 0; i <= 6; i++) {
        uint8_t mode = 1, rgb[3] = { 0x12, 0x34, 0x56 };
        kbd_colour_apply(i, &mode, rgb);
        TEST_ASSERT_EQUAL_INT(i, kbd_colour_index(mode, rgb));
    }
}

static void test_custom_is_minus_one(void)
{
    uint8_t rgb[3] = { 0x12, 0x34, 0x56 };
    TEST_ASSERT_EQUAL_INT(-1, kbd_colour_index(1, rgb));
    TEST_ASSERT_EQUAL_INT(0, kbd_colour_index(0, rgb));   /* mode 0: the keyboard's own, whatever rgb holds */
}

static void test_apply_keyboard_own_keeps_rgb(void)
{
    uint8_t mode = 1, rgb[3] = { 0xFF, 0x00, 0x00 };
    kbd_colour_apply(0, &mode, rgb);
    TEST_ASSERT_EQUAL_UINT8(0, mode);
    TEST_ASSERT_EQUAL_UINT8(0xFF, rgb[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, rgb[1]);
    TEST_ASSERT_EQUAL_UINT8(0x00, rgb[2]);
}

static void test_apply_out_of_range_no_change(void)
{
    const int bad[] = { 7, -1 };
    for (unsigned k = 0; k < 2; k++) {
        uint8_t mode = 1, rgb[3] = { 0x12, 0x34, 0x56 };
        kbd_colour_apply(bad[k], &mode, rgb);
        TEST_ASSERT_EQUAL_UINT8(1, mode);
        TEST_ASSERT_EQUAL_UINT8(0x12, rgb[0]);
        TEST_ASSERT_EQUAL_UINT8(0x34, rgb[1]);
        TEST_ASSERT_EQUAL_UINT8(0x56, rgb[2]);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_swatches_never_amber);
    RUN_TEST(test_index_round_trip);
    RUN_TEST(test_custom_is_minus_one);
    RUN_TEST(test_apply_keyboard_own_keeps_rgb);
    RUN_TEST(test_apply_out_of_range_no_change);
    return UNITY_END();
}
