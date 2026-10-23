#include "ssh/device_key.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"
#include "esp_console.h"
#include "sys/debug_console.h"
#include "settings/settings.h"

static const char *TAG = "devkey";
#define NS "devkey"
#define NVS_KEY "priv"

/* Wipe and free a libssh-allocated string that held private key text. */
static void scrub_free(char *s)
{
    if (!s) return;
    explicit_bzero(s, strlen(s));
    free(s);
}

static esp_err_t generate_and_store(void)
{
    ssh_key key = NULL;
    char *b64 = NULL;
    esp_err_t err = ESP_FAIL;
    if (ssh_pki_generate(SSH_KEYTYPE_ED25519, 0, &key) != SSH_OK) {
        ESP_LOGE(TAG, "ed25519 generate failed");
        return ESP_FAIL;
    }
    if (ssh_pki_export_privkey_base64(key, NULL, NULL, NULL, &b64) != SSH_OK || !b64) {
        ESP_LOGE(TAG, "private key export failed");
        ssh_key_free(key);
        return ESP_FAIL;
    }
    nvs_handle_t h;
    err = nvs_open(NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, NVS_KEY, b64);
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
    }
    scrub_free(b64);
    ssh_key_free(key);
    if (err != ESP_OK) ESP_LOGE(TAG, "store failed: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "generated a new device key");
    return err;
}

esp_err_t device_key_get(ssh_key *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NOT_FOUND;
    if (err != ESP_OK) return err;
    size_t len = 0;
    err = nvs_get_str(h, NVS_KEY, NULL, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return ESP_ERR_NOT_FOUND; }
    if (err != ESP_OK) { nvs_close(h); return err; }
    char *buf = malloc(len);
    if (!buf) { nvs_close(h); return ESP_ERR_NO_MEM; }
    err = nvs_get_str(h, NVS_KEY, buf, &len);
    nvs_close(h);
    if (err != ESP_OK) { explicit_bzero(buf, len); free(buf); return err; }
    int rc = ssh_pki_import_privkey_base64(buf, NULL, NULL, NULL, out);
    scrub_free(buf);
    if (rc != SSH_OK) {
        ESP_LOGE(TAG, "stored device key is unreadable; run 'sshkey regenerate' to replace it (this changes the public key)");
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t device_key_ensure(void)
{
    ssh_key k = NULL;
    esp_err_t err = device_key_get(&k);
    if (err == ESP_OK) { ssh_key_free(k); return ESP_OK; }
    if (err != ESP_ERR_NOT_FOUND) return err; /* never overwrite a key that exists but failed to load */
    return generate_and_store();
}

esp_err_t device_key_regenerate(void)
{
    return generate_and_store();
}

esp_err_t device_key_pubkey_line(char *out, size_t cap)
{
    if (!out || cap == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = device_key_ensure();
    if (err != ESP_OK) return err;
    ssh_key key = NULL;
    err = device_key_get(&key);
    if (err != ESP_OK) return err;
    char *b64 = NULL;
    if (ssh_pki_export_pubkey_base64(key, &b64) != SSH_OK || !b64) {
        ssh_key_free(key);
        return ESP_FAIL;
    }
    const char *node = g_settings.ts_device_name[0] ? g_settings.ts_device_name : "tab5";
    int n = snprintf(out, cap, "%s %s claude-companion@%s", ssh_key_type_to_char(ssh_key_type(key)), b64, node);
    free(b64);
    ssh_key_free(key);
    return (n > 0 && (size_t)n < cap) ? ESP_OK : ESP_ERR_NO_MEM;
}

static int cmd_sshkey(int argc, char **argv)
{
    if (argc > 2) { printf("usage: sshkey [regenerate]\n"); return 1; }
    if (argc == 2) {
        if (strcmp(argv[1], "regenerate") != 0) { printf("usage: sshkey [regenerate]\n"); return 1; }
        if (device_key_regenerate() != ESP_OK) { printf("sshkey: regenerate failed\n"); return 1; }
    }
    char line[256];
    if (device_key_pubkey_line(line, sizeof(line)) != ESP_OK) { printf("sshkey: failed\n"); return 1; }
    printf("@@PUBKEY %s\n", line);
    return 0;
}

void device_key_console_init(void)
{
    const esp_console_cmd_t cmd = {
        .command = "sshkey",
        .help = "Print the device SSH public key as '@@PUBKEY <line>'; 'sshkey regenerate' makes a new key",
        .hint = NULL,
        .func = cmd_sshkey,
    };
    debug_console_register(&cmd);
}
