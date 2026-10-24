#include "unity.h"
#include "servers/host_rules.h"
#include <string.h>
#include <stdint.h>

void setUp(void) {}
void tearDown(void) {}

static void test_hf_validate_ok(void)
{
    int port = 0;
    TEST_ASSERT_EQUAL(HF_OK, hf_validate("box.local", "22", "me", false, "mac.local", &port));
    TEST_ASSERT_EQUAL(22, port);
    TEST_ASSERT_EQUAL(HF_OK, hf_validate("box.local", "65535", "me", false, "", &port));
    TEST_ASSERT_EQUAL(65535, port);
    TEST_ASSERT_EQUAL(HF_OK, hf_validate("box.local", "1", "me", false, NULL, NULL));
}

static void test_hf_validate_port(void)
{
    const char *bad[] = { "0", "70000", "abc", "", "22x", "-22", " 22", "99999999999" };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        TEST_ASSERT_EQUAL_MESSAGE(HF_BAD_PORT, hf_validate("box.local", bad[i], "me", false, "mac.local", NULL), bad[i]);
    TEST_ASSERT_EQUAL(HF_BAD_PORT, hf_validate("box.local", NULL, "me", false, "mac.local", NULL));
}

static void test_hf_validate_required(void)
{
    TEST_ASSERT_EQUAL(HF_NO_ADDRESS, hf_validate("", "22", "me", false, "mac.local", NULL));
    TEST_ASSERT_EQUAL(HF_NO_ADDRESS, hf_validate("   ", "22", "me", false, "mac.local", NULL));
    TEST_ASSERT_EQUAL(HF_NO_ADDRESS, hf_validate(NULL, "22", "me", false, "mac.local", NULL));
    TEST_ASSERT_EQUAL(HF_NO_USER, hf_validate("box.local", "22", "", false, "mac.local", NULL));
    TEST_ASSERT_EQUAL(HF_NO_USER, hf_validate("box.local", "22", " ", false, "mac.local", NULL));
}

static void test_hf_validate_is_host(void)
{
    TEST_ASSERT_EQUAL(HF_IS_HOST, hf_validate("MAC.local.", "22", "me", false, "mac.local", NULL));
    TEST_ASSERT_EQUAL(HF_OK, hf_validate("MAC.local.", "22", "me", true, "mac.local", NULL));
    TEST_ASSERT_EQUAL(HF_OK, hf_validate("mac.local", "22", "me", false, "", NULL));   /* no Host set */
}

static void test_hf_auth_of(void)
{
    TEST_ASSERT_EQUAL(HF_AUTH_DEVICE, hf_auth_of(true, ""));
    TEST_ASSERT_EQUAL(HF_AUTH_DEVICE, hf_auth_of(true, "device"));
    TEST_ASSERT_EQUAL(HF_AUTH_DEVICE, hf_auth_of(true, NULL));
    TEST_ASSERT_EQUAL(HF_AUTH_SD_FILE, hf_auth_of(true, "/sdcard/id_ed25519"));
    TEST_ASSERT_EQUAL(HF_AUTH_PASSWORD, hf_auth_of(false, ""));
    TEST_ASSERT_EQUAL(HF_AUTH_PASSWORD, hf_auth_of(false, "/sdcard/id_ed25519"));
}

static void test_hf_tailnet_host(void)
{
    TEST_ASSERT_TRUE(hf_tailnet_host("100.64.0.1"));
    TEST_ASSERT_TRUE(hf_tailnet_host("box.tail1234.ts.net"));
    TEST_ASSERT_FALSE(hf_tailnet_host("100.128.0.1"));
    TEST_ASSERT_FALSE(hf_tailnet_host("192.168.1.5"));
    TEST_ASSERT_FALSE(hf_tailnet_host(NULL));
}

static void test_key_source_never_falls_back(void)
{
    TEST_ASSERT_EQUAL(HF_KEY_FILE_MISSING, hf_key_source("/sdcard/id", false));
    TEST_ASSERT_NOT_EQUAL(HF_KEY_DEVICE, hf_key_source("/sdcard/id", false));
    TEST_ASSERT_EQUAL(HF_KEY_FILE, hf_key_source("/sdcard/id", true));
    TEST_ASSERT_EQUAL(HF_KEY_DEVICE, hf_key_source("", false));
    TEST_ASSERT_EQUAL(HF_KEY_DEVICE, hf_key_source("device", false));
    TEST_ASSERT_EQUAL(HF_KEY_DEVICE, hf_key_source(NULL, false));
    TEST_ASSERT_NOT_EQUAL(HF_KEY_DEVICE, hf_key_source("/spiffs/id", false));   /* any file path: never the device key */
    TEST_ASSERT_TRUE(hf_key_on_sd("/sdcard/id"));
    TEST_ASSERT_FALSE(hf_key_on_sd("/sdcardx/id"));
    TEST_ASSERT_FALSE(hf_key_on_sd("device"));
    TEST_ASSERT_FALSE(hf_key_on_sd(NULL));
}

/* C1: the Host by any address the link knows (invented addresses only) */
static void test_hf_is_host_any_address(void)
{
    hf_host_addrs_t h;
    hf_host_addrs_init(&h);
    hf_host_addrs_add(&h, "studio.local");
    hf_host_addrs_add(&h, "192.168.77.20");   /* the LAN fallback */
    hf_host_addrs_add(&h, "100.64.9.9");      /* the carry / mac_ts_ip tailnet address */
    hf_host_addrs_add(&h, "");                /* ignored */
    hf_host_addrs_add(&h, NULL);
    hf_host_addrs_add(&h, "100.64.9.9");      /* no duplicate */
    TEST_ASSERT_EQUAL(3, h.n);
    TEST_ASSERT_TRUE(hf_is_host("100.64.9.9", &h));
    TEST_ASSERT_TRUE(hf_is_host("192.168.77.20", &h));
    TEST_ASSERT_TRUE(hf_is_host("STUDIO.local.", &h));
    TEST_ASSERT_TRUE(hf_is_host("studio", &h));                 /* the short name */
    TEST_ASSERT_FALSE(hf_is_host("100.64.9.10", &h));
    TEST_ASSERT_FALSE(hf_is_host("", &h));
    TEST_ASSERT_FALSE(hf_is_host("box.local", NULL));
    TEST_ASSERT_EQUAL(HF_IS_HOST, hf_validate_addrs("100.64.9.9", "22", "me", false, &h, NULL));
    TEST_ASSERT_EQUAL(HF_IS_HOST, hf_validate_addrs("192.168.77.20", "22", "me", false, &h, NULL));
    TEST_ASSERT_EQUAL(HF_OK, hf_validate_addrs("192.168.77.20", "22", "me", true, &h, NULL));
    TEST_ASSERT_EQUAL(HF_OK, hf_validate_addrs("192.168.77.21", "22", "me", false, &h, NULL));
    for (int i = 0; i < 40; i++) hf_host_addrs_add(&h, "10.0.0.1");   /* never overflows */
    TEST_ASSERT_TRUE(h.n <= HF_HOST_ADDRS_MAX);
}

/* I1: a name another entry has is refused (case-insensitive); the edited entry itself is not "another" */
static void test_hf_name_taken(void)
{
    const char *names[] = { "Studio", "pi-lan", "build-box" };
    TEST_ASSERT_TRUE(hf_name_taken("studio", names, 3, -1));
    TEST_ASSERT_TRUE(hf_name_taken("PI-LAN", names, 3, 0));
    TEST_ASSERT_FALSE(hf_name_taken("pi-lan", names, 3, 1));   /* itself */
    TEST_ASSERT_FALSE(hf_name_taken("new-box", names, 3, -1));
    TEST_ASSERT_FALSE(hf_name_taken("", names, 3, -1));
    TEST_ASSERT_FALSE(hf_name_taken("x", NULL, 0, -1));
}

/* M9: a key file only under /sdcard/, never with ".." */
static void test_key_path_confined(void)
{
    TEST_ASSERT_EQUAL(HF_KEY_BAD_PATH, hf_key_source("/spiffs/id", true));
    TEST_ASSERT_EQUAL(HF_KEY_BAD_PATH, hf_key_source("/sdcard/../nvs/key", true));
    TEST_ASSERT_EQUAL(HF_KEY_BAD_PATH, hf_key_source("/sdcard/ssh/..", true));
    TEST_ASSERT_EQUAL(HF_KEY_BAD_PATH, hf_key_source("id_ed25519", true));
    TEST_ASSERT_EQUAL(HF_KEY_FILE, hf_key_source("/sdcard/ssh/id..backup", true));   /* ".." only as a part */
    TEST_ASSERT_EQUAL(HF_KEY_FILE_MISSING, hf_key_source("/sdcard/id", false));
}

/* M10 + M11: authenticate's plan (the seam ssh_client.c uses): a key entry never tries a password, and a key file
   never becomes the device key */
static void test_auth_plan(void)
{
    hf_auth_plan_t p = hf_auth_plan(true, "/sdcard/id", false);
    TEST_ASSERT_EQUAL(HF_KEY_FILE_MISSING, p.key);
    TEST_ASSERT_FALSE(p.try_password);
    p = hf_auth_plan(true, "/sdcard/id", true);
    TEST_ASSERT_EQUAL(HF_KEY_FILE, p.key);
    TEST_ASSERT_FALSE(p.try_password);
    p = hf_auth_plan(true, "device", false);
    TEST_ASSERT_EQUAL(HF_KEY_DEVICE, p.key);
    TEST_ASSERT_FALSE(p.try_password);
    p = hf_auth_plan(false, "/sdcard/id", true);
    TEST_ASSERT_EQUAL(HF_KEY_NONE, p.key);
    TEST_ASSERT_TRUE(p.try_password);
}

/* P4C-10: the Host is the server that presents any key the companion link ever pinned (a FIFO of 8) */
static void key(uint8_t out[32], uint8_t v) { memset(out, v, 32); }
static void test_hostkey_is_host(void)
{
    uint8_t list[HF_HOSTKEYS_MAX][32], k[32];
    int n = 0;
    key(k, 1); n = hf_hostkeys_add(list, n, k);
    key(k, 2); n = hf_hostkeys_add(list, n, k);
    key(k, 1); n = hf_hostkeys_add(list, n, k);   /* no duplicate */
    TEST_ASSERT_EQUAL(2, n);
    key(k, 1); TEST_ASSERT_TRUE(hf_hostkey_is_host(k, list, n));
    key(k, 2); TEST_ASSERT_TRUE(hf_hostkey_is_host(k, list, n));
    key(k, 3); TEST_ASSERT_FALSE(hf_hostkey_is_host(k, list, n));
    TEST_ASSERT_FALSE(hf_hostkey_is_host(k, NULL, 0));
    key(k, 0); TEST_ASSERT_EQUAL(n, hf_hostkeys_add(list, n, k));   /* an all-zero hash is no key */
    TEST_ASSERT_FALSE(hf_hostkey_is_host(k, list, n));
}
static void test_hostkeys_fifo(void)
{
    uint8_t list[HF_HOSTKEYS_MAX][32], k[32];
    int n = 0;
    for (int i = 1; i <= HF_HOSTKEYS_MAX + 2; i++) { key(k, (uint8_t)i); n = hf_hostkeys_add(list, n, k); }
    TEST_ASSERT_EQUAL(HF_HOSTKEYS_MAX, n);
    key(k, 1); TEST_ASSERT_FALSE(hf_hostkey_is_host(k, list, n));   /* the two oldest went */
    key(k, 2); TEST_ASSERT_FALSE(hf_hostkey_is_host(k, list, n));
    key(k, 3); TEST_ASSERT_TRUE(hf_hostkey_is_host(k, list, n));
    key(k, HF_HOSTKEYS_MAX + 2); TEST_ASSERT_TRUE(hf_hostkey_is_host(k, list, n));
    key(k, 3); n = hf_hostkeys_add(list, n, k);   /* re-pinning an old key keeps it (moves it to the newest) */
    key(k, 100); n = hf_hostkeys_add(list, n, k);
    key(k, 3); TEST_ASSERT_TRUE(hf_hostkey_is_host(k, list, n));
}
/* C1 convenience: a .local Host name also matches its peer's MagicDNS name by the first label (invented names) */
static void test_host_addrs_first_label(void)
{
    hf_host_addrs_t h;
    hf_host_addrs_init(&h);
    hf_host_addrs_add_name(&h, "studio.local");
    TEST_ASSERT_TRUE(hf_is_host("studio.tail1234.ts.net", &h));
    TEST_ASSERT_TRUE(hf_is_host("STUDIO.tail1234.ts.net.", &h));
    TEST_ASSERT_FALSE(hf_is_host("studio2.tail1234.ts.net", &h));
    TEST_ASSERT_FALSE(hf_is_host("box.tail1234.ts.net", &h));
    hf_host_addrs_init(&h);
    hf_host_addrs_add_name(&h, "192.168.77.20");   /* an IP has no label */
    TEST_ASSERT_EQUAL(1, h.n);
    TEST_ASSERT_FALSE(hf_is_host("192.tail1234.ts.net", &h));
}

/* I-new-2: the deny-list read has three results; only NONE and FOUND let a shell through, and ERROR never writes */
static void test_shell_gate(void)
{
    uint8_t list[HF_HOSTKEYS_MAX][32], host[32], other[32];
    key(host, 7); key(other, 9);
    int n = hf_hostkeys_add(list, 0, host);
    const uint8_t (*l)[32] = (const uint8_t (*)[32])list;
    TEST_ASSERT_EQUAL(HF_GATE_ALLOW, hf_shell_gate(true, HF_DENY_FOUND, other, l, n));
    TEST_ASSERT_EQUAL(HF_GATE_REFUSE_HOST, hf_shell_gate(true, HF_DENY_FOUND, host, l, n));
    TEST_ASSERT_EQUAL(HF_GATE_ALLOW, hf_shell_gate(true, HF_DENY_NONE, host, l, 0));   /* no list yet */
    TEST_ASSERT_EQUAL(HF_GATE_REFUSE_UNCHECKED, hf_shell_gate(true, HF_DENY_ERROR, other, l, 0));
    TEST_ASSERT_EQUAL(HF_GATE_REFUSE_UNCHECKED, hf_shell_gate(true, HF_DENY_FOUND, NULL, l, n));   /* no server key */
    TEST_ASSERT_EQUAL(HF_GATE_ALLOW, hf_shell_gate(false, HF_DENY_ERROR, host, l, n));   /* the take-over (cmd) */
    TEST_ASSERT_TRUE(hf_hostkeys_may_write(HF_DENY_NONE));
    TEST_ASSERT_TRUE(hf_hostkeys_may_write(HF_DENY_FOUND));
    TEST_ASSERT_FALSE(hf_hostkeys_may_write(HF_DENY_ERROR));
}
/* the gate runs before the host-key prompt and before any authentication (the steps ssh_task runs, in order) */
static int step_at(const hf_step_t *s, int n, hf_step_t want) { for (int i = 0; i < n; i++) if (s[i] == want) return i; return -1; }
static void test_conn_steps_gate_first(void)
{
    hf_step_t s[8];
    int n = hf_conn_steps(true, s, 8);
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL(0, step_at(s, n, HF_STEP_DENY_CHECK));
    TEST_ASSERT_TRUE(step_at(s, n, HF_STEP_DENY_CHECK) < step_at(s, n, HF_STEP_VERIFY_HOST));
    TEST_ASSERT_TRUE(step_at(s, n, HF_STEP_VERIFY_HOST) < step_at(s, n, HF_STEP_AUTH));
    n = hf_conn_steps(false, s, 8);
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL(-1, step_at(s, n, HF_STEP_DENY_CHECK));
    TEST_ASSERT_EQUAL(HF_STEP_AUTH, s[n - 1]);
    TEST_ASSERT_EQUAL(0, hf_conn_steps(true, s, 2));   /* too small: nothing to run, never a partial order */
}
/* M-new-5: the first label counts only against MagicDNS names and a bare name */
static void test_first_label_scope(void)
{
    hf_host_addrs_t h;
    hf_host_addrs_init(&h);
    hf_host_addrs_add_name(&h, "web.local");
    TEST_ASSERT_FALSE(hf_is_host("web.example.com", &h));
    TEST_ASSERT_FALSE(hf_is_host("web.lan", &h));
    TEST_ASSERT_TRUE(hf_is_host("web.tail1234.ts.net", &h));
    TEST_ASSERT_TRUE(hf_is_host("WEB", &h));
    TEST_ASSERT_TRUE(hf_is_host("web.", &h));
    TEST_ASSERT_TRUE(hf_is_host("web.local", &h));
    TEST_ASSERT_FALSE(hf_is_host("webx.tail1234.ts.net", &h));
    char l[64];
    TEST_ASSERT_TRUE(hf_first_label(" web.local", l, sizeof(l)));
    TEST_ASSERT_EQUAL_STRING("web", l);
    TEST_ASSERT_FALSE(hf_first_label("10.1.2.3", l, sizeof(l)));
    TEST_ASSERT_FALSE(hf_first_label("web", l, sizeof(l)));   /* no dot: the name is itself */
}
/* key-type hardening: the type the link met first, then libssh's defaults (a reorder only) */
static void test_hostkeys_pref(void)
{
    char b[400];
    TEST_ASSERT_TRUE(hf_hostkeys_pref("ssh-rsa", b, sizeof(b)));
    TEST_ASSERT_EQUAL(0, strncmp(b, "rsa-sha2-512,rsa-sha2-256,ssh-ed25519,", 38));
    TEST_ASSERT_EQUAL(1, (int)(strstr(b + 1, "rsa-sha2-512") == NULL));   /* no repeat */
    TEST_ASSERT_TRUE(hf_hostkeys_pref("ecdsa-sha2-nistp256", b, sizeof(b)));
    TEST_ASSERT_EQUAL(0, strncmp(b, "ecdsa-sha2-nistp256,ssh-ed25519,ecdsa-sha2-nistp521,ecdsa-sha2-nistp384,sk-", 75));
    TEST_ASSERT_TRUE(hf_hostkeys_pref("ssh-ed25519", b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING(HF_DEFAULT_HOSTKEYS, b);
    TEST_ASSERT_FALSE(hf_hostkeys_pref("", b, sizeof(b)));
    TEST_ASSERT_FALSE(hf_hostkeys_pref("ssh-dss", b, sizeof(b)));
    TEST_ASSERT_FALSE(hf_hostkeys_pref("ssh-rsa", b, 10));   /* too small: no option rather than a cut list */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hf_validate_ok);
    RUN_TEST(test_hf_validate_port);
    RUN_TEST(test_hf_validate_required);
    RUN_TEST(test_hf_validate_is_host);
    RUN_TEST(test_hf_auth_of);
    RUN_TEST(test_hf_tailnet_host);
    RUN_TEST(test_key_source_never_falls_back);
    RUN_TEST(test_hf_is_host_any_address);
    RUN_TEST(test_hf_name_taken);
    RUN_TEST(test_key_path_confined);
    RUN_TEST(test_auth_plan);
    RUN_TEST(test_hostkey_is_host);
    RUN_TEST(test_hostkeys_fifo);
    RUN_TEST(test_host_addrs_first_label);
    RUN_TEST(test_shell_gate);
    RUN_TEST(test_conn_steps_gate_first);
    RUN_TEST(test_first_label_scope);
    RUN_TEST(test_hostkeys_pref);
    return UNITY_END();
}
