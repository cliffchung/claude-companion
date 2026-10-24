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
    TEST_ASSERT_EQUAL(HF_KEY_FILE_MISSING, hf_key_source("/spiffs/id", false));   /* any file path */
    TEST_ASSERT_TRUE(hf_key_on_sd("/sdcard/id"));
    TEST_ASSERT_FALSE(hf_key_on_sd("/sdcardx/id"));
    TEST_ASSERT_FALSE(hf_key_on_sd("device"));
    TEST_ASSERT_FALSE(hf_key_on_sd(NULL));
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
    return UNITY_END();
}
