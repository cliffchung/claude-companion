#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include "unity.h"
#include "ssh/cb_gate.h"

/* The gate's lock is a real mutex here (portMUX on the device), and its nap really sleeps, so a second thread can be
   inside a callback while the owner detaches and waits. */
static pthread_mutex_t s_mx = PTHREAD_MUTEX_INITIALIZER;
static void mx_lock(void *ctx) { (void)ctx; pthread_mutex_lock(&s_mx); }
static void mx_unlock(void *ctx) { (void)ctx; pthread_mutex_unlock(&s_mx); }
static void nap(void *ctx, int ms) { (void)ctx; struct timespec t = { 0, (long)ms * 1000000L }; nanosleep(&t, NULL); }
static const cb_gate_ops_t s_ops = { mx_lock, mx_unlock, nap, NULL };

void setUp(void) {}
void tearDown(void) {}

static void test_enter_exit_counts(void)
{
    cb_gate_t g = { 0 };
    TEST_ASSERT_TRUE(cb_gate_enter(&g, &s_ops));
    TEST_ASSERT_TRUE(cb_gate_enter(&g, &s_ops));
    TEST_ASSERT_EQUAL_INT(2, g.busy);
    cb_gate_exit(&g, &s_ops);
    cb_gate_exit(&g, &s_ops);
    TEST_ASSERT_EQUAL_INT(0, g.busy);
}

static void test_no_callback_after_detach(void)
{
    cb_gate_t g = { 0 };
    TEST_ASSERT_TRUE(cb_gate_detach_wait(&g, &s_ops, 100));   /* nothing in flight: at once */
    TEST_ASSERT_TRUE(g.detached);
    TEST_ASSERT_FALSE(cb_gate_enter(&g, &s_ops));
    TEST_ASSERT_EQUAL_INT(0, g.busy);   /* a refused entry isn't counted */
}

/* A callback on another thread: enters, says so, works for hold_ms, records that it finished, exits. */
typedef struct { cb_gate_t *g; int hold_ms; atomic_int entered, finished; } flight_t;
static void *flight(void *arg)
{
    flight_t *f = arg;
    if (!cb_gate_enter(f->g, &s_ops)) return NULL;
    atomic_store(&f->entered, 1);
    nap(NULL, f->hold_ms);
    atomic_store(&f->finished, 1);
    cb_gate_exit(f->g, &s_ops);
    return NULL;
}

/* I1: the owner's detach returns only once the callback in flight has left (the owner may then free its context). */
static void test_detach_waits_for_callback_in_flight(void)
{
    cb_gate_t g = { 0 };
    flight_t f = { .g = &g, .hold_ms = 150 };
    pthread_t t;
    pthread_create(&t, NULL, flight, &f);
    while (!atomic_load(&f.entered)) nap(NULL, 1);
    bool idle = cb_gate_detach_wait(&g, &s_ops, 2000);
    TEST_ASSERT_TRUE(idle);
    TEST_ASSERT_EQUAL_INT(1, atomic_load(&f.finished));   /* never returns while the callback still runs */
    TEST_ASSERT_FALSE(cb_gate_enter(&g, &s_ops));
    pthread_join(t, NULL);
}

/* A callback that outlasts the bound: the wait gives up (false), so the owner must not free its context. */
static void test_detach_wait_is_bounded(void)
{
    cb_gate_t g = { 0 };
    flight_t f = { .g = &g, .hold_ms = 400 };
    pthread_t t;
    pthread_create(&t, NULL, flight, &f);
    while (!atomic_load(&f.entered)) nap(NULL, 1);
    TEST_ASSERT_FALSE(cb_gate_detach_wait(&g, &s_ops, 50));
    TEST_ASSERT_EQUAL_INT(0, atomic_load(&f.finished));
    pthread_join(t, NULL);
    TEST_ASSERT_EQUAL_INT(0, g.busy);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_enter_exit_counts);
    RUN_TEST(test_no_callback_after_detach);
    RUN_TEST(test_detach_waits_for_callback_in_flight);
    RUN_TEST(test_detach_wait_is_bounded);
    return UNITY_END();
}
