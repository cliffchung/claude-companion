/* server_store over simulated NVS and card (Plan 4 Task 11): NVS is the only store, and the CFG 23 import adds
   the TF card's hosts once without touching the card. Replaces the fork's unbuilt tools/ui_preview test. */
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "servers/server_store.h"
#include "settings/settings.h"
#include "crypto/secure_store.h"
#include "storage/sd.h"
#include "servers/host_rules.h"

static char *s_nvs;            /* "srv"/"list" */
static char *s_card;           /* /sdcard/.tab5/servers.enc, NULL = no file */
static bool s_mounted, s_fail_write;
static int s_nvs_writes, s_card_reads;

bool nvs_set_str_ns(const char *ns, const char *key, const char *v)
{
    TEST_ASSERT_EQUAL_STRING("srv", ns);
    TEST_ASSERT_EQUAL_STRING("list", key);
    if (s_fail_write) return false;
    free(s_nvs); s_nvs = strdup(v); s_nvs_writes++;
    return true;
}
bool nvs_get_str_alloc(const char *ns, const char *key, char **out)
{
    (void)ns; (void)key;
    *out = s_nvs ? strdup(s_nvs) : NULL;
    return *out != NULL;
}
/* "enc:" + text stands in for the device key's cipher; anything else doesn't decrypt */
char *secure_encrypt_str(const char *p)
{
    char *o = malloc(strlen(p) + 5);
    strcpy(o, "enc:"); strcat(o, p);
    return o;
}
char *secure_decrypt_str(const char *b) { return strncmp(b, "enc:", 4) ? NULL : strdup(b + 4); }
bool sd_is_mounted(void) { return s_mounted; }
bool fs_read_all(const char *path, char **out, size_t *len, size_t max)
{
    (void)max;
    TEST_ASSERT_EQUAL_STRING("/sdcard/.tab5/servers.enc", path);
    if (!s_mounted || !s_card) return false;
    s_card_reads++;
    *out = strdup(s_card); if (len) *len = strlen(*out);
    return true;
}

static void set_str(char **slot, const char *v) { free(*slot); *slot = v ? strdup(v) : NULL; }

void setUp(void)
{
    set_str(&s_nvs, NULL); set_str(&s_card, NULL);
    s_mounted = true; s_fail_write = false; s_nvs_writes = 0; s_card_reads = 0;
    server_store_init();
}
void tearDown(void) {}

static server_t entry(const char *name, const char *host, const char *pass)
{
    server_t s = { .port = 22 };
    strcpy(s.name, name); strcpy(s.host, host); strcpy(s.user, "tester"); strcpy(s.pass, pass);
    return s;
}

/* The card file as an older build wrote it: the list's JSON (passwords encrypted), encrypted as a whole. */
static void card_with(const char *list_json) { char *c = secure_encrypt_str(list_json); set_str(&s_card, c); free(c); }

#define CARD_TWO "{\"servers\":[" \
    "{\"name\":\"pi\",\"host\":\"pi.local\",\"port\":2222,\"user\":\"pi\",\"pass\":\"enc:test-only\",\"key\":\"\",\"use_key\":false}," \
    "{\"name\":\"nas\",\"host\":\"nas.local\",\"port\":22,\"user\":\"admin\",\"pass\":\"\",\"key\":\"/sdcard/id_nas\",\"use_key\":true}]}"

static void test_nvs_round_trip(void)
{
    server_t a = entry("first", "one.example", "test-only");
    TEST_ASSERT_EQUAL_INT(0, server_store_add(&a));
    TEST_ASSERT_NOT_NULL(strstr(s_nvs, "enc:test-only"));   /* the password is saved encrypted */
    server_store_init();   /* a reboot */
    TEST_ASSERT_EQUAL_INT(1, server_store_count());
    TEST_ASSERT_EQUAL_STRING("one.example", server_store_get(0)->host);
    TEST_ASSERT_EQUAL_STRING("test-only", server_store_get(0)->pass);
    TEST_ASSERT_EQUAL_INT(0, s_card_reads);   /* init never reads the card */
}

static void test_init_does_not_write(void)
{
    server_t a = entry("first", "one.example", "test-only");
    server_store_add(&a);
    int w = s_nvs_writes;
    server_store_init();
    TEST_ASSERT_EQUAL_INT(w, s_nvs_writes);
}

static void test_failed_write_rolls_back(void)
{
    server_t a = entry("first", "one.example", "test-only");
    server_store_add(&a);
    s_fail_write = true;
    server_t b = entry("second", "two.example", "");
    TEST_ASSERT_EQUAL_INT(-1, server_store_add(&b));
    TEST_ASSERT_FALSE(server_store_update(0, &b));
    TEST_ASSERT_EQUAL_STRING("one.example", server_store_get(0)->host);
    TEST_ASSERT_FALSE(server_store_remove(0));
    TEST_ASSERT_EQUAL_INT(1, server_store_count());
}

static void test_import_adds_card_hosts_once(void)
{
    server_t host = entry("Host", "host.local", "");
    server_store_add(&host);
    card_with(CARD_TWO);
    char *card_before = strdup(s_card);
    TEST_ASSERT_EQUAL_INT(2, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(3, server_store_count());
    TEST_ASSERT_EQUAL_STRING("Host", server_store_get(0)->name);   /* the NVS list stays first */
    TEST_ASSERT_EQUAL_STRING("pi.local", server_store_get(1)->host);
    TEST_ASSERT_EQUAL_INT(2222, server_store_get(1)->port);
    TEST_ASSERT_EQUAL_STRING("test-only", server_store_get(1)->pass);
    TEST_ASSERT_TRUE(server_store_get(2)->use_key);
    TEST_ASSERT_EQUAL_STRING("/sdcard/id_nas", server_store_get(2)->keypath);
    TEST_ASSERT_EQUAL_STRING(card_before, s_card);   /* the card file is untouched */
    free(card_before);
    server_store_init();   /* saved to NVS */
    TEST_ASSERT_EQUAL_INT(3, server_store_count());
    /* run again: the same entries are skipped */
    TEST_ASSERT_EQUAL_INT(0, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(3, server_store_count());
}

static void test_import_skips_same_name_or_box(void)
{
    server_t a = entry("pi", "other.example", "");          /* the same name */
    server_t b = entry("my-nas", "NAS.local", "");          /* the same user, host (any case) and port */
    strcpy(b.user, "admin");
    server_store_add(&a); server_store_add(&b);
    card_with(CARD_TWO);
    TEST_ASSERT_EQUAL_INT(0, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(2, server_store_count());
    TEST_ASSERT_EQUAL_STRING("other.example", server_store_get(0)->host);
}

static void test_import_no_card_or_file_keeps_list(void)
{
    server_t host = entry("Host", "host.local", "");
    server_store_add(&host);
    char *nvs_before = strdup(s_nvs);
    card_with(CARD_TWO);
    s_mounted = false;                                        /* no card */
    TEST_ASSERT_EQUAL_INT(-1, server_store_import_sd_once());
    s_mounted = true; set_str(&s_card, NULL);                 /* a card without the file */
    TEST_ASSERT_EQUAL_INT(-1, server_store_import_sd_once());
    set_str(&s_card, "v1:another-device");                    /* doesn't decrypt */
    TEST_ASSERT_EQUAL_INT(-1, server_store_import_sd_once());
    card_with("not json");                                    /* isn't a list */
    TEST_ASSERT_EQUAL_INT(-1, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(1, server_store_count());
    TEST_ASSERT_EQUAL_STRING(nvs_before, s_nvs);
    free(nvs_before);
}

static void test_import_failed_save_keeps_list(void)
{
    server_t host = entry("Host", "host.local", "");
    server_store_add(&host);
    card_with(CARD_TWO);
    s_fail_write = true;
    TEST_ASSERT_EQUAL_INT(-1, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(1, server_store_count());
}

static void test_import_stops_at_full_list(void)
{
    for (int i = 0; i < SERVER_MAX - 1; i++) {
        char n[16], h[32];
        snprintf(n, sizeof(n), "h%d", i); snprintf(h, sizeof(h), "h%d.example", i);
        server_t e = entry(n, h, "");
        TEST_ASSERT_EQUAL_INT(i, server_store_add(&e));
    }
    card_with(CARD_TWO);
    TEST_ASSERT_EQUAL_INT(1, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(SERVER_MAX, server_store_count());
}

/* I2: names are one case-insensitive namespace (hf_name_taken), the Host's included */
static void test_import_skips_case_only_name(void)
{
    server_t host = entry("Desk", "studio.local", "");
    server_t pi = entry("PI", "other.example", "");
    server_store_add(&host); server_store_add(&pi);
    card_with("{\"servers\":["
              "{\"name\":\"desk\",\"host\":\"else.example\",\"port\":22,\"user\":\"u\"},"
              "{\"name\":\"pi\",\"host\":\"pi.local\",\"port\":2222,\"user\":\"pi\"}]}");
    TEST_ASSERT_EQUAL_INT(0, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(2, server_store_count());
}

/* M5: a card list repeating itself (by name in any case, or by user, host and port) adds each entry once */
static void test_import_card_self_duplicates(void)
{
    card_with("{\"servers\":["
              "{\"name\":\"pi\",\"host\":\"pi.local\",\"port\":22,\"user\":\"pi\"},"
              "{\"name\":\"PI\",\"host\":\"other.local\",\"port\":22,\"user\":\"pi\"},"
              "{\"name\":\"pi2\",\"host\":\"PI.local\",\"port\":22,\"user\":\"pi\"},"
              "{\"name\":\"nas\",\"host\":\"nas.local\",\"port\":22,\"user\":\"admin\"}]}");
    TEST_ASSERT_EQUAL_INT(2, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(2, server_store_count());
    TEST_ASSERT_EQUAL_STRING("pi.local", server_store_get(0)->host);
    TEST_ASSERT_EQUAL_STRING("nas.local", server_store_get(1)->host);
}

/* M4: the form's rules (hf_validate: an address, a user, port 1..65535) and a key file only under /sdcard/ */
static void test_import_skips_invalid_entries(void)
{
    card_with("{\"servers\":["
              "{\"name\":\"p0\",\"host\":\"a.local\",\"port\":0,\"user\":\"u\"},"
              "{\"name\":\"pbig\",\"host\":\"b.local\",\"port\":70000,\"user\":\"u\"},"
              "{\"name\":\"pneg\",\"host\":\"c.local\",\"port\":-22,\"user\":\"u\"},"
              "{\"name\":\"nouser\",\"host\":\"d.local\",\"port\":22,\"user\":\"  \"},"
              "{\"name\":\"blank\",\"host\":\"   \",\"port\":22,\"user\":\"u\"},"
              "{\"name\":\"etc\",\"host\":\"e.local\",\"port\":22,\"user\":\"u\",\"key\":\"/etc/id\",\"use_key\":true},"
              "{\"name\":\"dots\",\"host\":\"f.local\",\"port\":22,\"user\":\"u\",\"key\":\"/sdcard/../id\",\"use_key\":true},"
              "{\"name\":\"dev\",\"host\":\"g.local\",\"port\":22,\"user\":\"u\",\"key\":\"device\",\"use_key\":true},"
              "{\"name\":\"file\",\"host\":\"h.local\",\"port\":65535,\"user\":\"u\",\"key\":\"/sdcard/id_h\",\"use_key\":true},"
              "{\"name\":\"pw\",\"host\":\"i.local\",\"port\":1,\"user\":\"u\",\"key\":\"/etc/unused\",\"use_key\":false}]}");
    TEST_ASSERT_EQUAL_INT(3, server_store_import_sd_once());
    TEST_ASSERT_EQUAL_INT(3, server_store_count());
    TEST_ASSERT_EQUAL_STRING("dev", server_store_get(0)->name);
    TEST_ASSERT_EQUAL_STRING("file", server_store_get(1)->name);
    TEST_ASSERT_EQUAL_STRING("pw", server_store_get(2)->name);   /* a password entry never reads its key path */
}

/* M5: an entry carrying the Host's address is imported, not skipped: at this point of the boot the link's
   addresses aren't known (no tailnet lookup, no host cache, the Host's own entry may be one of the card's), so
   the runtime rule (cc_link_is_companion_server: hf_is_host over cc_link_host_addresses) hides it from Terminal and
   refuses its shell. The Host's own entry from the card comes in too. */
static void test_import_entry_with_host_address(void)
{
    card_with("{\"servers\":["
              "{\"name\":\"Desk\",\"host\":\"studio.local\",\"port\":22,\"user\":\"me\"},"
              "{\"name\":\"studio-root\",\"host\":\"STUDIO.local\",\"port\":22,\"user\":\"root\"}]}");
    TEST_ASSERT_EQUAL_INT(2, server_store_import_sd_once());
    hf_host_addrs_t h;
    hf_host_addrs_init(&h);
    hf_host_addrs_add_name(&h, server_store_get(0)->host);   /* the Host's entry, as cc_link_host_addresses adds it */
    TEST_ASSERT_TRUE(hf_is_host(server_store_get(1)->host, &h));   /* so Terminal hides and refuses it */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nvs_round_trip);
    RUN_TEST(test_init_does_not_write);
    RUN_TEST(test_failed_write_rolls_back);
    RUN_TEST(test_import_adds_card_hosts_once);
    RUN_TEST(test_import_skips_same_name_or_box);
    RUN_TEST(test_import_no_card_or_file_keeps_list);
    RUN_TEST(test_import_failed_save_keeps_list);
    RUN_TEST(test_import_stops_at_full_list);
    RUN_TEST(test_import_skips_case_only_name);
    RUN_TEST(test_import_card_self_duplicates);
    RUN_TEST(test_import_skips_invalid_entries);
    RUN_TEST(test_import_entry_with_host_address);
    set_str(&s_nvs, NULL); set_str(&s_card, NULL);
    return UNITY_END();
}
