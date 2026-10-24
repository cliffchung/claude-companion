#include "unity.h"
#include "net/ts_host.h"

void setUp(void) {}
void tearDown(void) {}

static void test_ipv4(void)
{
    TEST_ASSERT_TRUE(ts_host_is_ipv4("192.168.1.50"));
    TEST_ASSERT_TRUE(ts_host_is_ipv4("100.64.0.1"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4("192.168.1"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4("192.168.1.256"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4("192.168.1.2.3"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4("192.168.01x.2"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4("1..2.3"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4("my-host.local"));
    TEST_ASSERT_FALSE(ts_host_is_ipv4(""));
    TEST_ASSERT_FALSE(ts_host_is_ipv4(NULL));
}

static void test_tailnet_ip(void)
{
    TEST_ASSERT_TRUE(ts_host_tailnet_ip("100.64.0.0"));
    TEST_ASSERT_TRUE(ts_host_tailnet_ip("100.101.102.103"));
    TEST_ASSERT_TRUE(ts_host_tailnet_ip("100.127.255.255"));
    TEST_ASSERT_FALSE(ts_host_tailnet_ip("100.128.0.1"));
    TEST_ASSERT_FALSE(ts_host_tailnet_ip("100.63.255.255"));
    TEST_ASSERT_FALSE(ts_host_tailnet_ip("192.168.1.50"));
    TEST_ASSERT_FALSE(ts_host_tailnet_ip("my-host.ts.net"));
}

/* The SSH terminal's "Tailscale is off" check: a tailnet address or a *.ts.net name. */
static void test_needs_tailscale(void)
{
    TEST_ASSERT_TRUE(ts_host_needs_tailscale("100.101.102.103"));
    TEST_ASSERT_TRUE(ts_host_needs_tailscale("pi.tail1234.ts.net"));
    TEST_ASSERT_TRUE(ts_host_needs_tailscale("PI.TAIL1234.TS.NET"));
    TEST_ASSERT_TRUE(ts_host_needs_tailscale("pi.tail1234.ts.net."));
    TEST_ASSERT_FALSE(ts_host_needs_tailscale("192.168.1.50"));
    TEST_ASSERT_FALSE(ts_host_needs_tailscale("my-host.local"));
    TEST_ASSERT_FALSE(ts_host_needs_tailscale("ts.net.example.com"));
    TEST_ASSERT_FALSE(ts_host_needs_tailscale("ts.net"));
    TEST_ASSERT_FALSE(ts_host_needs_tailscale(""));
    TEST_ASSERT_FALSE(ts_host_needs_tailscale(NULL));
}

/* M2/L48: the terminal's refusal is a pure check, so `ts term-test` never starts a client (no Host command). */
static void test_term_refusal(void)
{
    TEST_ASSERT_EQUAL_STRING(TS_HOST_OFF_MESSAGE, ts_host_term_refusal("100.101.102.103", false));
    TEST_ASSERT_EQUAL_STRING(TS_HOST_OFF_MESSAGE, ts_host_term_refusal("pi.tail1234.ts.net", false));
    TEST_ASSERT_NULL(ts_host_term_refusal("100.101.102.103", true));
    TEST_ASSERT_NULL(ts_host_term_refusal("192.168.1.50", false));
    TEST_ASSERT_NULL(ts_host_term_refusal("my-host.local", false));
    TEST_ASSERT_NULL(ts_host_term_refusal(NULL, false));
}

/* A check-in brings the tunnel up only for a host that may be on the tailnet: a tailnet address, a
   *.ts.net name or a bare MagicDNS name. A LAN address, a .local name or another DNS name never waits on it. */
static void test_maybe_tailnet(void)
{
    TEST_ASSERT_TRUE(ts_host_maybe_tailnet("100.101.102.103"));
    TEST_ASSERT_TRUE(ts_host_maybe_tailnet("pi.tail1234.ts.net"));
    TEST_ASSERT_TRUE(ts_host_maybe_tailnet("my-host"));
    TEST_ASSERT_FALSE(ts_host_maybe_tailnet("192.168.1.50"));
    TEST_ASSERT_FALSE(ts_host_maybe_tailnet("my-host.local"));
    TEST_ASSERT_FALSE(ts_host_maybe_tailnet("my-host.LOCAL."));
    TEST_ASSERT_FALSE(ts_host_maybe_tailnet("mac.example.com"));
    TEST_ASSERT_FALSE(ts_host_maybe_tailnet(""));
    TEST_ASSERT_FALSE(ts_host_maybe_tailnet(NULL));
}

static void test_mdns(void)
{
    TEST_ASSERT_TRUE(ts_host_is_mdns("my-host.local"));
    TEST_ASSERT_TRUE(ts_host_is_mdns("my-host.Local."));
    TEST_ASSERT_FALSE(ts_host_is_mdns("local"));
    TEST_ASSERT_FALSE(ts_host_is_mdns(".local"));
    TEST_ASSERT_FALSE(ts_host_is_mdns("my-host.localdomain"));
    TEST_ASSERT_FALSE(ts_host_is_mdns(NULL));
}

/* The Mac's fallback address: a private IPv4 LAN address, never a tailnet one; "" clears it. */
static void test_lan_fallback_valid(void)
{
    TEST_ASSERT_TRUE(ts_host_lan_fallback_valid("192.168.1.50"));
    TEST_ASSERT_TRUE(ts_host_lan_fallback_valid("10.0.0.5"));
    TEST_ASSERT_TRUE(ts_host_lan_fallback_valid("172.16.4.4"));
    TEST_ASSERT_TRUE(ts_host_lan_fallback_valid(""));
    TEST_ASSERT_FALSE(ts_host_lan_fallback_valid("172.32.0.1"));
    TEST_ASSERT_FALSE(ts_host_lan_fallback_valid("100.101.102.103"));
    TEST_ASSERT_FALSE(ts_host_lan_fallback_valid("8.8.8.8"));
    TEST_ASSERT_FALSE(ts_host_lan_fallback_valid("my-host.local"));
    TEST_ASSERT_FALSE(ts_host_lan_fallback_valid(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ipv4);
    RUN_TEST(test_tailnet_ip);
    RUN_TEST(test_needs_tailscale);
    RUN_TEST(test_term_refusal);
    RUN_TEST(test_maybe_tailnet);
    RUN_TEST(test_mdns);
    RUN_TEST(test_lan_fallback_valid);
    return UNITY_END();
}
