#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "cc_watch.h"

#define BODY_CAP (256 * 1024)

static char *s_body;
static cc_watch_t s_w;

void setUp(void)
{
    s_body = malloc(BODY_CAP);
    cc_watch_init(&s_w, s_body, BODY_CAP);
}

void tearDown(void)
{
    free(s_body);
}

/* Feeds all of data in one go per call, collecting every event into evs. */
static int feed_all(cc_watch_t *w, const char *data, size_t n, cc_watch_ev_t *evs, int max)
{
    int k = 0;
    while (n > 0) {
        size_t used = 0;
        cc_watch_ev_t ev = cc_watch_feed(w, data, n, &used);
        TEST_ASSERT_TRUE(used <= n);
        data += used;
        n -= used;
        if (ev == CC_WATCH_NONE) {
            TEST_ASSERT_EQUAL_size_t(0, n);
            break;
        }
        TEST_ASSERT_TRUE(k < max);
        evs[k++] = ev;
    }
    return k;
}

#define SAMPLE "@@CC-WATCH 1\r\n@@CC-BEGIN 0\r\n[1]\r\n@@CC-END\r\n@@CC-HB 63\r\n"

static void test_banner_block_heartbeat(void)
{
    const char *in = SAMPLE;
    size_t n = strlen(in), used = 0;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BANNER, cc_watch_feed(&s_w, in, n, &used));
    in += used; n -= used;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, cc_watch_feed(&s_w, in, n, &used));
    TEST_ASSERT_EQUAL_STRING("[1]\n", s_w.body);
    TEST_ASSERT_EQUAL_size_t(4, s_w.len);
    TEST_ASSERT_EQUAL_INT(0, s_w.rc);
    in += used; n -= used;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_HEARTBEAT, cc_watch_feed(&s_w, in, n, &used));
    TEST_ASSERT_EQUAL_size_t(n, used);
}

static void test_feed_bytewise(void)
{
    const char *in = SAMPLE;
    cc_watch_ev_t got[8];
    int k = 0;
    for (size_t i = 0; in[i]; i++) {
        size_t used = 0;
        cc_watch_ev_t ev = cc_watch_feed(&s_w, in + i, 1, &used);
        TEST_ASSERT_EQUAL_size_t(1, used);
        if (ev != CC_WATCH_NONE) {
            got[k++] = ev;
            if (ev == CC_WATCH_BLOCK) {
                TEST_ASSERT_EQUAL_STRING("[1]\n", s_w.body);
                TEST_ASSERT_EQUAL_INT(0, s_w.rc);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT(3, k);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BANNER, got[0]);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, got[1]);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_HEARTBEAT, got[2]);
}

static void test_ignores_echo_and_noise(void)
{
    const char *in = "\r\n\n   \nx@@CC-END\r\n@@CC-END\r\n@@CC-WATCH 1 \r\n@@CC-HBx\r\n"
                     "@@CC-WATCH 1\r\n\r\nnoise\r\n@@CC-BEGIN 0\r\n[2]\r\n@@CC-END\r\n";
    cc_watch_ev_t evs[8];
    int k = feed_all(&s_w, in, strlen(in), evs, 8);
    TEST_ASSERT_EQUAL_INT(2, k);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BANNER, evs[0]);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, evs[1]);
    TEST_ASSERT_EQUAL_STRING("[2]\n", s_w.body);
}

static void test_long_body_line(void)
{
    size_t line = 200000;
    char *in = malloc(line + 64);
    size_t o = 0;
    o += (size_t)sprintf(in, "@@CC-BEGIN 0\r\n");
    in[o++] = '[';
    memset(in + o, '1', line - 2);
    o += line - 2;
    in[o++] = ']';
    o += (size_t)sprintf(in + o, "\r\n@@CC-END\r\n");
    cc_watch_ev_t evs[4];
    int k = feed_all(&s_w, in, o, evs, 4);
    TEST_ASSERT_EQUAL_INT(1, k);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, evs[0]);
    TEST_ASSERT_EQUAL_size_t(200001, s_w.len);
    TEST_ASSERT_EQUAL_size_t(200001, strlen(s_w.body));
    TEST_ASSERT_EQUAL_CHAR('[', s_w.body[0]);
    TEST_ASSERT_EQUAL_CHAR(']', s_w.body[199999]);
    TEST_ASSERT_EQUAL_CHAR('\n', s_w.body[200000]);
    free(in);
}

static void test_overflow(void)
{
    char small[64];
    cc_watch_t w;
    cc_watch_init(&w, small, sizeof(small));
    char in[512];
    size_t o = (size_t)sprintf(in, "@@CC-BEGIN 0\r\n");
    memset(in + o, 'a', 100);
    o += 100;
    o += (size_t)sprintf(in + o, "\r\n@@CC-END\r\n@@CC-BEGIN 0\r\n[3]\r\n@@CC-END\r\n");
    cc_watch_ev_t evs[4];
    int k = feed_all(&w, in, o, evs, 4);
    TEST_ASSERT_EQUAL_INT(2, k);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_OVERFLOW, evs[0]);
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, evs[1]);
    TEST_ASSERT_EQUAL_STRING("[3]\n", w.body);
}

static void test_rc_and_bye(void)
{
    const char *in = "@@CC-BEGIN 127\r\nsh: claude: not found\r\n@@CC-END\r\n@@CC-BYE quiet\r\n";
    size_t n = strlen(in), used = 0;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, cc_watch_feed(&s_w, in, n, &used));
    TEST_ASSERT_EQUAL_INT(127, s_w.rc);
    TEST_ASSERT_EQUAL_STRING("sh: claude: not found\n", s_w.body);
    in += used; n -= used;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BYE, cc_watch_feed(&s_w, in, n, &used));
    TEST_ASSERT_EQUAL_STRING("quiet", s_w.bye);
    TEST_ASSERT_EQUAL_size_t(n, used);
}

static void test_two_events_one_feed(void)
{
    const char *in = "@@CC-BEGIN 0\r\n[4]\r\n@@CC-END\r\n@@CC-BEGIN 0\r\n[5]\r\n@@CC-END\r\n";
    size_t n = strlen(in), used = 0;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, cc_watch_feed(&s_w, in, n, &used));
    TEST_ASSERT_TRUE(used < n);
    TEST_ASSERT_EQUAL_STRING("[4]\n", s_w.body);
    in += used; n -= used;
    TEST_ASSERT_EQUAL_INT(CC_WATCH_BLOCK, cc_watch_feed(&s_w, in, n, &used));
    TEST_ASSERT_EQUAL_size_t(n, used);
    TEST_ASSERT_EQUAL_STRING("[5]\n", s_w.body);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_banner_block_heartbeat);
    RUN_TEST(test_feed_bytewise);
    RUN_TEST(test_ignores_echo_and_noise);
    RUN_TEST(test_long_body_line);
    RUN_TEST(test_overflow);
    RUN_TEST(test_rc_and_bye);
    RUN_TEST(test_two_events_one_feed);
    return UNITY_END();
}
