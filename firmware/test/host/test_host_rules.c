#include "unity.h"
#include "servers/host_rules.h"

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
    return UNITY_END();
}
