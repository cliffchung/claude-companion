#include <string.h>
#include "unity.h"
#include "net/fast_rules.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- Step 4: the Host address cache ---- */

static hc_cache_t hc(const char *entry, const char *ip)
{
    hc_cache_t c;
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_TRUE(hc_on_resolved(&c, entry, ip));
    return c;
}

static void test_hc_used_for_its_entry(void)
{
    hc_cache_t c = hc("my-host.local", "192.168.1.50");
    TEST_ASSERT_TRUE(hc_use(&c, "my-host.local", false, false));
    TEST_ASSERT_FALSE(hc_use(&c, "other.local", false, false));
    TEST_ASSERT_FALSE(hc_use(&c, "", false, false));
    TEST_ASSERT_FALSE(hc_use(&c, NULL, false, false));
    TEST_ASSERT_FALSE(hc_use(NULL, "my-host.local", false, false));
}

/* a literal address has nothing to look up */
static void test_hc_literal_entry(void)
{
    hc_cache_t c;
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_FALSE(hc_on_resolved(&c, "192.168.1.50", "192.168.1.50"));
    TEST_ASSERT_FALSE(hc_use(&c, "192.168.1.50", false, false));
}

/* a pending Forget host key always looks the name up: the forget's pin must go to the real Host */
static void test_hc_not_during_forget(void)
{
    hc_cache_t c = hc("my-host.local", "192.168.1.50");
    TEST_ASSERT_FALSE(hc_use(&c, "my-host.local", false, true));
}

/* a cached tailnet address with the switch off is never used (the link never starts Tailscale) */
static void test_hc_tailnet_needs_switch(void)
{
    hc_cache_t c = hc("my-host", "100.64.0.18");
    TEST_ASSERT_FALSE(hc_use(&c, "my-host", false, false));
    TEST_ASSERT_TRUE(hc_use(&c, "my-host", true, false));
}

static void test_hc_bad_values(void)
{
    hc_cache_t c;
    memset(&c, 0, sizeof(c));
    TEST_ASSERT_FALSE(hc_on_resolved(&c, "my-host.local", "not-an-ip"));
    TEST_ASSERT_FALSE(hc_use(&c, "my-host.local", false, false));
    TEST_ASSERT_FALSE(hc_on_resolved(&c, "", "192.168.1.50"));
    c = hc("my-host.local", "192.168.1.50");
    strcpy(c.ip, "192.168.1");
    TEST_ASSERT_FALSE(hc_use(&c, "my-host.local", false, false));
}

static void test_hc_change_reported(void)
{
    hc_cache_t c = hc("my-host.local", "192.168.1.50");
    TEST_ASSERT_FALSE(hc_on_resolved(&c, "my-host.local", "192.168.1.50"));
    TEST_ASSERT_TRUE(hc_on_resolved(&c, "my-host.local", "192.168.1.51"));
    TEST_ASSERT_TRUE(hc_on_resolved(&c, "mac.local", "192.168.1.51"));
}

/* The cached address only skips the lookup: it is kept only when its own pin matches (KH_OK). Every other
   result (unknown, changed, unreadable) is dropped without acting on it, and the normal lookup decides. */
static void test_hc_only_a_matching_pin_counts(void)
{
    TEST_ASSERT_EQUAL(HC_ACCEPT, hc_cached_key(KH_OK));
    TEST_ASSERT_EQUAL(HC_LOOKUP, hc_cached_key(KH_UNKNOWN));
    TEST_ASSERT_EQUAL(HC_LOOKUP, hc_cached_key(KH_CHANGED));
    TEST_ASSERT_EQUAL(HC_LOOKUP, hc_cached_key(KH_ERROR));
    TEST_ASSERT_EQUAL(HC_LOOKUP, hc_cached_key(KH_PIN_ERROR));
}

/* ---- Step 3: the static address setting, "address/prefix gateway [dns]" ---- */

static uint32_t ip4(int a, int b, int c, int d)
{
    uint8_t x[4] = { (uint8_t)a, (uint8_t)b, (uint8_t)c, (uint8_t)d };
    uint32_t v;
    memcpy(&v, x, 4);   /* network order, as lwIP keeps it */
    return v;
}

static void test_sip_parse(void)
{
    sip_t s;
    TEST_ASSERT_TRUE(sip_parse("192.168.1.50/24 192.168.1.1", &s));
    TEST_ASSERT_EQUAL_HEX32(ip4(192, 168, 1, 50), s.ip);
    TEST_ASSERT_EQUAL_HEX32(ip4(255, 255, 255, 0), s.mask);
    TEST_ASSERT_EQUAL_HEX32(ip4(192, 168, 1, 1), s.gw);
    TEST_ASSERT_EQUAL_HEX32(ip4(192, 168, 1, 1), s.dns);   /* no DNS given: the gateway */
    TEST_ASSERT_TRUE(sip_parse("  10.0.0.7/8   10.0.0.1  1.1.1.1 ", &s));
    TEST_ASSERT_EQUAL_HEX32(ip4(255, 0, 0, 0), s.mask);
    TEST_ASSERT_EQUAL_HEX32(ip4(1, 1, 1, 1), s.dns);
    TEST_ASSERT_TRUE(sip_parse("192.168.1.50/30 192.168.1.49", &s));
    TEST_ASSERT_EQUAL_HEX32(ip4(255, 255, 255, 252), s.mask);
}

static void test_sip_parse_rejects(void)
{
    sip_t s;
    TEST_ASSERT_FALSE(sip_parse("", &s));
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50", &s));                  /* no prefix, no gateway */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/24", &s));               /* no gateway */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/0 192.168.1.1", &s));
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/31 192.168.1.51", &s));  /* too small for a gateway */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/24 10.0.0.1", &s));      /* gateway off the subnet */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/24 192.168.1.50", &s));  /* gateway is the address */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.0/24 192.168.1.1", &s));    /* the network address */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.255/24 192.168.1.1", &s));  /* the broadcast address */
    TEST_ASSERT_FALSE(sip_parse("192.168.1.500/24 192.168.1.1", &s));
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/24 192.168.1.1 dns", &s));
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/24 192.168.1.1 1.1.1.1 extra", &s));
    TEST_ASSERT_FALSE(sip_parse("192.168.1.50/2x 192.168.1.1", &s));
    TEST_ASSERT_FALSE(sip_parse(NULL, &s));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hc_used_for_its_entry);
    RUN_TEST(test_hc_literal_entry);
    RUN_TEST(test_hc_not_during_forget);
    RUN_TEST(test_hc_tailnet_needs_switch);
    RUN_TEST(test_hc_bad_values);
    RUN_TEST(test_hc_change_reported);
    RUN_TEST(test_hc_only_a_matching_pin_counts);
    RUN_TEST(test_sip_parse);
    RUN_TEST(test_sip_parse_rejects);
    return UNITY_END();
}
