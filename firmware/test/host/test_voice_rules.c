#include <stdio.h>
#include <string.h>
#include "unity.h"
#include "voice/voice_rules.h"

void setUp(void) {}
void tearDown(void) {}

static void test_wav_header_exact(void)
{
    uint8_t h[VR_WAV_HDR];
    static const uint8_t want[44] = {
        'R','I','F','F', 0x24,0x7D,0x00,0x00, 'W','A','V','E', 'f','m','t',' ', 0x10,0,0,0, 1,0, 1,0,
        0x80,0x3E,0,0, 0x00,0x7D,0,0, 2,0, 0x10,0, 'd','a','t','a', 0x00,0x7D,0,0 };
    TEST_ASSERT_EQUAL(44, vr_wav_header(h, 32000, 16000, 1, 16));
    TEST_ASSERT_EQUAL_MEMORY(want, h, 44);
}

static void test_wav_header_rejects(void)
{
    uint8_t h[VR_WAV_HDR];
    TEST_ASSERT_EQUAL(0, vr_wav_header(h, 10, 0, 1, 16));
    TEST_ASSERT_EQUAL(0, vr_wav_header(h, 10, 16000, 0, 16));
    TEST_ASSERT_EQUAL(0, vr_wav_header(h, 10, 16000, 1, 8));
}

static void test_downmix_average_pick_saturate(void)
{
    int16_t in[4] = {100, 300, -100, -300}, out[2];
    TEST_ASSERT_EQUAL(2, vr_downmix(in, 2, 2, -1, out));
    TEST_ASSERT_EQUAL(200, out[0]); TEST_ASSERT_EQUAL(-200, out[1]);
    vr_downmix(in, 2, 2, 0, out);
    TEST_ASSERT_EQUAL(100, out[0]); TEST_ASSERT_EQUAL(-100, out[1]);
    vr_downmix(in, 2, 2, 1, out);
    TEST_ASSERT_EQUAL(300, out[0]); TEST_ASSERT_EQUAL(-300, out[1]);
    int16_t hi[2] = {32767, 32767}, lo[2] = {-32768, -32768};
    vr_downmix(hi, 1, 2, -1, out); TEST_ASSERT_EQUAL(32767, out[0]);
    vr_downmix(lo, 1, 2, -1, out); TEST_ASSERT_EQUAL(-32768, out[0]);
}

static void test_rms_dbfs(void)
{
    int16_t z[8] = {0}, a[8], b[8];
    for (int i = 0; i < 8; i++) { a[i] = (i & 1) ? -32767 : 32767; b[i] = (i & 1) ? -3277 : 3277; }
    TEST_ASSERT_EQUAL(-96, vr_rms_dbfs(z, 8));
    TEST_ASSERT_EQUAL(0, vr_rms_dbfs(a, 8));
    TEST_ASSERT_EQUAL(-20, vr_rms_dbfs(b, 8));
    TEST_ASSERT_EQUAL(-96, vr_rms_dbfs(a, 0));
}

static void test_meter_mapping(void)
{
    TEST_ASSERT_EQUAL(0, vr_meter(-60)); TEST_ASSERT_EQUAL(50, vr_meter(-30));
    TEST_ASSERT_EQUAL(100, vr_meter(0)); TEST_ASSERT_EQUAL(0, vr_meter(-90));
    TEST_ASSERT_EQUAL(100, vr_meter(5));
}

static void test_heard_threshold(void)
{
    TEST_ASSERT_TRUE(vr_heard(-50));
    TEST_ASSERT_FALSE(vr_heard(-51));
}

static void test_toggle_press_then_tap_stops(void)
{
    vr_keys_t k = {0};
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 0));
    TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, true, 400));
    TEST_ASSERT_EQUAL(VR_KEY_STOP, vr_on_voice_key(&k, true, 800));
}

static void test_toggle_held_key_starts_once(void)
{
    vr_keys_t k = {0};
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 0));
    for (int64_t t = 500; t <= 3000; t += 60) TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, true, t));
    TEST_ASSERT_EQUAL(VR_KEY_STOP, vr_on_voice_key(&k, true, 3400));
}

static void test_toggle_hold_after_stop_does_not_restart(void)
{
    vr_keys_t k = {0};
    vr_on_voice_key(&k, false, 0);
    for (int64_t t = 500; t <= 3000; t += 60) vr_on_voice_key(&k, true, t);
    TEST_ASSERT_EQUAL(VR_KEY_STOP, vr_on_voice_key(&k, true, 3400));
    for (int64_t t = 3900; t <= 5000; t += 60) TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, false, t));
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 5500));
}

/* review M7: the stop press meant for a recording the service already ended (the 30 s limit) starts nothing */
static void test_toggle_stop_after_auto_stop_is_ignored(void)
{
    vr_keys_t k = {0};
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 0));
    vr_on_voice_ended(&k, 30000);                                       /* the limit: no longer recording */
    TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, false, 30300));   /* the user's late "stop" */
    TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, false, 30650));   /* still inside the guard */
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 31400));    /* a deliberate press later starts */
}

static void test_toggle_ended_after_user_stop_keeps_guard(void)
{
    vr_keys_t k = {0};
    vr_on_voice_key(&k, false, 0);
    TEST_ASSERT_EQUAL(VR_KEY_STOP, vr_on_voice_key(&k, true, 3400));
    vr_on_voice_ended(&k, 3450);                 /* the UI sees the stop take effect a moment later */
    vr_on_voice_ended(&k, 3000);                 /* an older time never moves the guard back */
    TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, false, 4100));
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 4300));
}

static void test_toggle_ended_before_any_key(void)
{
    vr_keys_t k = {0};
    vr_on_voice_ended(&k, 1000);                 /* a console recording ended while Sessions watched */
    TEST_ASSERT_EQUAL(VR_KEY_IGNORE, vr_on_voice_key(&k, false, 1200));
    TEST_ASSERT_EQUAL(VR_KEY_START, vr_on_voice_key(&k, false, 1800));
}

static void clean_is(const char *in, const char *want)
{
    char buf[128];
    strcpy(buf, in);
    size_t n = vr_clean_transcript(buf);
    TEST_ASSERT_EQUAL_STRING(want, buf);
    TEST_ASSERT_EQUAL(strlen(want), n);
}

static void test_clean_leading_space(void) { clean_is(" Please run the tests.\n", "Please run the tests."); }

static void test_clean_tags(void)
{
    clean_is("[BLANK_AUDIO]\n", ""); clean_is("(blender whirring)\n", ""); clean_is(" [MUSIC] ", "");
    clean_is("*coughs*", ""); clean_is(" .\n", "");
    clean_is("Hello [MUSIC] world.", "Hello world.");
}

static void test_clean_multiline_and_controls(void)
{
    clean_is("First.\nSecond.\n", "First. Second.");
    clean_is("a\x1b\x7f\x01" "b\tc", "ab c");
}

static void test_clean_keeps_utf8_and_unclosed(void)
{
    clean_is("caf\xc3\xa9 [unclosed ", "caf\xc3\xa9 [unclosed");
}

/* spec 9.3 step 4: symbols are not letters, so whisper's music notes, ellipses, dashes and curly quotes alone empty
   it (Task 2 review minor); a real letter beside them keeps everything, and non-Latin letters still count */
static void test_clean_symbols_only(void)
{
    clean_is("\xe2\x99\xaa\n", "");                            /* ♪ */
    clean_is(" \xe2\x99\xaa \xe2\x99\xab ", "");               /* ♪ ♫ */
    clean_is("\xe2\x80\xa6", "");                               /* … */
    clean_is("\xe2\x80\x93 \xe2\x80\x94", "");                 /* – — */
    clean_is("\xe2\x80\x9c\xe2\x80\x9d \xe2\x80\x98\xe2\x80\x99", "");   /* “” ‘’ */
    clean_is("\xe2\x99\xaa Hello \xe2\x99\xaa", "\xe2\x99\xaa Hello \xe2\x99\xaa");
    clean_is("\xe2\x80\x9c" "7\xe2\x80\x9d", "\xe2\x80\x9c" "7\xe2\x80\x9d");
    clean_is("\xc3\xa9", "\xc3\xa9");                            /* é */
    clean_is("\xe6\x97\xa5\xe6\x9c\xac", "\xe6\x97\xa5\xe6\x9c\xac");     /* 日本 */
}

static void test_merge_spacing(void)
{
    char o[64];
    TEST_ASSERT_EQUAL(VR_MERGE_OK, vr_merge_draft("", "Hi", o, 63)); TEST_ASSERT_EQUAL_STRING("Hi", o);
    TEST_ASSERT_EQUAL(VR_MERGE_OK, vr_merge_draft("abc", "Def", o, 63)); TEST_ASSERT_EQUAL_STRING("abc Def", o);
    TEST_ASSERT_EQUAL(VR_MERGE_OK, vr_merge_draft("abc ", "Def", o, 63)); TEST_ASSERT_EQUAL_STRING("abc Def", o);
    TEST_ASSERT_EQUAL(VR_MERGE_EMPTY, vr_merge_draft("abc", "", o, 63)); TEST_ASSERT_EQUAL_STRING("abc", o);
}

static void test_merge_cut_at_word(void)
{
    char d[471], o[481], want[481];
    memset(d, 'x', 470); d[470] = 0;
    TEST_ASSERT_EQUAL(VR_MERGE_CUT, vr_merge_draft(d, "one two three", o, VR_TEXT_MAX));
    snprintf(want, sizeof want, "%s one two", d);
    TEST_ASSERT_EQUAL(478, strlen(o));
    TEST_ASSERT_EQUAL_STRING(want, o);
}

static void test_merge_cut_utf8_boundary(void)
{
    char t[31], o[64];
    for (int i = 0; i < 10; i++) memcpy(t + 3 * i, "\xe2\x82\xac", 3);
    t[30] = 0;
    TEST_ASSERT_EQUAL(VR_MERGE_CUT, vr_merge_draft("ab", t, o, 12));   /* "ab " + 9 bytes */
    TEST_ASSERT_EQUAL(12, strlen(o));
    TEST_ASSERT_EQUAL(0, strlen(o + 3) % 3);
    TEST_ASSERT_EQUAL_MEMORY("\xe2\x82\xac\xe2\x82\xac\xe2\x82\xac", o + 3, 9);
    TEST_ASSERT_EQUAL(VR_MERGE_CUT, vr_merge_draft("ab", t, o, 13));   /* 10 bytes avail: 3 chars */
    TEST_ASSERT_EQUAL(12, strlen(o));
}

static void test_host_result_map(void)
{
    TEST_ASSERT_EQUAL(VR_OUT_TEXT, vr_host_result(0));
    TEST_ASSERT_EQUAL(VR_OUT_NO_WHISPER, vr_host_result(120));
    TEST_ASSERT_EQUAL(VR_OUT_NO_MODEL, vr_host_result(121));
    TEST_ASSERT_EQUAL(VR_OUT_BAD_MODEL, vr_host_result(3));
    TEST_ASSERT_EQUAL(VR_OUT_AUDIO_BAD, vr_host_result(11));
    TEST_ASSERT_EQUAL(VR_OUT_CANT_RUN, vr_host_result(126));
    TEST_ASSERT_EQUAL(VR_OUT_CANT_RUN, vr_host_result(127));
    TEST_ASSERT_EQUAL(VR_OUT_FAILED, vr_host_result(1));
    TEST_ASSERT_EQUAL(VR_OUT_FAILED, vr_host_result(2));
    TEST_ASSERT_EQUAL(VR_OUT_FAILED, vr_host_result(-1));
}

static void test_out_text_ids(void)
{
    for (int o = 0; o <= VR_OUT_CANCELLED; o++) {
        if (o == VR_OUT_TEXT || o == VR_OUT_STARTED || o == VR_OUT_CANCELLED) {
            TEST_ASSERT_EQUAL(STR_COUNT, vr_out_text((vr_out_t)o));
            continue;
        }
        str_id_t id = vr_out_text((vr_out_t)o);
        TEST_ASSERT_TRUE(id < STR_COUNT);
        TEST_ASSERT_TRUE(strlen(tr(id)) > 0);
        TEST_ASSERT_NULL(strstr(tr(id), "Mac"));
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wav_header_exact); RUN_TEST(test_wav_header_rejects);
    RUN_TEST(test_downmix_average_pick_saturate); RUN_TEST(test_rms_dbfs);
    RUN_TEST(test_meter_mapping); RUN_TEST(test_heard_threshold);
    RUN_TEST(test_toggle_press_then_tap_stops); RUN_TEST(test_toggle_held_key_starts_once);
    RUN_TEST(test_toggle_hold_after_stop_does_not_restart);
    RUN_TEST(test_toggle_stop_after_auto_stop_is_ignored); RUN_TEST(test_toggle_ended_after_user_stop_keeps_guard);
    RUN_TEST(test_toggle_ended_before_any_key);
    RUN_TEST(test_clean_leading_space); RUN_TEST(test_clean_tags);
    RUN_TEST(test_clean_multiline_and_controls); RUN_TEST(test_clean_keeps_utf8_and_unclosed);
    RUN_TEST(test_clean_symbols_only);
    RUN_TEST(test_merge_spacing); RUN_TEST(test_merge_cut_at_word); RUN_TEST(test_merge_cut_utf8_boundary);
    RUN_TEST(test_host_result_map); RUN_TEST(test_out_text_ids);
    return UNITY_END();
}
