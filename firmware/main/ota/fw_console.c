/* The `ota` console command (Plan 6 Task 5): `ota check` and `ota path` (read-only on the Host), and the debug
   `ota read-test`. Prints sizes, versions, SHA-256 prefixes, durations and exit codes only. */
#include "sdkconfig.h"
#include "ota/fw_console.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "mbedtls/sha256.h"
#include "sys/debug_console.h"
#include "companion/cc_link.h"
#include "companion/cc_cmd.h"
#include "ota/fw_rules.h"
#include "i18n/i18n.h"

static void hex12(const char *sha, char out[13]) { memcpy(out, sha, 12); out[12] = 0; }

static int cmd_check(void)
{
    uint8_t buf[CC_FW_INFO_MAX];
    size_t len = 0;
    int rc = -1;
    esp_err_t e = cc_link_fw_info(buf, sizeof(buf), &len, &rc);
    if (e != ESP_OK) { printf("ota check: %s, rc %d\n", esp_err_to_name(e), rc); return 1; }
    uint32_t size = 0;
    char sha[65] = "", p[13];
    const uint8_t *hdr = NULL;
    size_t hl = 0;
    if (rc != 0 || !fw_parse_info(buf, len, &size, sha, &hdr, &hl)) {
        printf("ota check: rc %d, %u B of output, not an info line\n", rc, (unsigned)len);
        return 1;
    }
    fw_desc_t d;
    memset(&d, 0, sizeof d);
    fw_hdr_t h = fw_check_header(hdr, hl, size, FW_SLOT_SIZE, (uint16_t)efuse_hal_chip_revision(), &d);
    hex12(sha, p);
    printf("ota check: rc %d, %u B, sha256 %s\xe2\x80\xa6, header %s, version %s\n", rc, (unsigned)size, p,
           fw_hdr_name(h), d.version);
    return 0;
}

static int cmd_path(int argc, char **argv)
{
    char path[64];
    if (argc == 3) {
        if (!cc_link_set_fw_path(argv[2])) { printf("ota path: %s\n", tr(STR_SET_CLAUDE_PATH_BAD)); return 1; }
        printf("ota path: ok\n");
        return 0;
    }
    printf("ota path: %s\n", cc_settings_fw_path(path, sizeof(path)));
    return 0;
}

#if CONFIG_COMPANION_DEBUG_COMMANDS
/* `ota read-test [cancel-at <pct>]`: the binary read alone, into PSRAM, hashed and freed. */
static volatile uint32_t s_rt_progress;
static volatile bool s_rt_cancel;
static volatile bool s_rt_done;
static uint32_t s_rt_size;
static int s_rt_cancel_pct;

static void cancel_helper(void *arg)
{
    (void)arg;
    while (!s_rt_done) {
        if (s_rt_size && fw_pct(s_rt_progress, s_rt_size) >= s_rt_cancel_pct) {
            printf("ota read-test: cancel at %u B\n", (unsigned)s_rt_progress);
            s_rt_cancel = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    vTaskDelete(NULL);
}

static int cmd_read_test(int argc, char **argv)
{
    int cancel_at = -1;
    if (argc == 4 && strcmp(argv[2], "cancel-at") == 0) cancel_at = atoi(argv[3]);
    else if (argc != 2) { printf("usage: ota read-test [cancel-at <pct>]\n"); return 1; }
    uint8_t info[CC_FW_INFO_MAX];
    size_t len = 0;
    int rc = -1;
    uint32_t size = 0;
    char sha[65];
    const uint8_t *hdr;
    size_t hl;
    esp_err_t e = cc_link_fw_info(info, sizeof(info), &len, &rc);
    if (e != ESP_OK || rc != 0 || !fw_parse_info(info, len, &size, sha, &hdr, &hl)) {
        printf("ota read-test: no info (%s, rc %d)\n", esp_err_to_name(e), rc);
        return 1;
    }
    uint8_t *buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { printf("ota read-test: no PSRAM for %u B\n", (unsigned)size + 1); return 1; }
    s_rt_progress = 0; s_rt_cancel = false; s_rt_done = false; s_rt_size = size; s_rt_cancel_pct = cancel_at;
    if (cancel_at >= 0) xTaskCreate(cancel_helper, "ota_rt_cancel", 3072, NULL, 5, NULL);
    int64_t t0 = esp_timer_get_time();
    size_t got = 0;
    e = cc_link_fw_read(buf, (size_t)size + 1, &got, &s_rt_progress, &s_rt_cancel, &rc, CC_FW_READ_TIMEOUT_MS(size));
    int ms = (int)((esp_timer_get_time() - t0) / 1000);
    s_rt_done = true;
    uint8_t h[32];
    mbedtls_sha256(buf, got, h, 0);
    char hx[65];
    for (int i = 0; i < 32; i++) snprintf(hx + 2 * i, 3, "%02x", h[i]);
    heap_caps_free(buf);
    printf("ota read-test: %s, rc %d, %u B in %d ms (%u KB/s), sha256 %s\n", esp_err_to_name(e), rc, (unsigned)got, ms,
           ms > 0 ? (unsigned)((uint64_t)got * 1000 / 1024 / (unsigned)ms) : 0u,
           got == size && strcmp(hx, sha) == 0 ? "match" : "MISMATCH");
    return e == ESP_OK ? 0 : 1;
}
#endif

static int cmd_ota(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "check") == 0) return cmd_check();
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "path") == 0) return cmd_path(argc, argv);
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc >= 2 && strcmp(argv[1], "read-test") == 0) return cmd_read_test(argc, argv);
    printf("usage: ota check | ota path [<path>] | ota read-test [cancel-at <pct>]\n");
#else
    printf("usage: ota check | ota path [<path>]\n");
#endif
    return 1;
}

void fw_console_init(void)
{
    const esp_console_cmd_t c = {
        .command = "ota",
        .help = "ota check | ota path [<path>]: the firmware image on the Host (Plan 6)",
        .func = cmd_ota,
    };
    debug_console_register(&c);
}
