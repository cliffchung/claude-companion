/* The `ota` console command (Plan 6 Tasks 5 and 6, spec §8.7): `ota status`, `ota check` and `ota path` (read-only on
   the Host), and the debug `ota read-test`, `update [corrupt-ram]`, `cancel`, `go-back`, `test-bootloop <n>`. Prints sizes, versions, SHA-256 prefixes, durations and exit codes only. */
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
#include "ota/fw_update.h"
#include "ui/ui.h"
#include "ui/ui_fwupdate.h"
#include "i18n/i18n.h"

static void hex12(const char *sha, char out[13]) { memcpy(out, sha, 12); out[12] = 0; }

static const char *offer_name(fw_offer_t o)
{
    switch (o) {
    case FW_OFFER_UP_TO_DATE: return "up to date";
    case FW_OFFER_NEWER:      return "newer";
    case FW_OFFER_OLDER:      return "older";
    default:                  return "other build";
    }
}

/* `ota check` (Task 6): the same check as About, read-only on the Host */
static int cmd_check(void)
{
    fwu_check_t c;
    fwu_check(&c);
    if (c.out != FW_OUT_OK) { printf("ota check: %s, rc %d\n", tr(fw_out_text(c.out)), c.rc); return 1; }
    char p[13];
    hex12(c.sha, p);
    printf("ota check: host %s, %u B, sha256 %s\xe2\x80\xa6, %s\n", c.desc.version, (unsigned)c.size, p,
           offer_name(c.offer));
    return 0;
}

static int cmd_status(void)
{
    fwu_info_t i;
    fwu_info(&i);
    fw_desc_t prev;
    bool has_prev = fwu_previous(&prev);
    printf("ota: running %s (%s, %s), other %s (%s, %s), rollback possible: %s, pending: %s, bootloop %d\n",
           fwu_running_version(), i.running_label, fwu_state_name(i.running_state), has_prev ? prev.version : "none",
           i.other_label, fwu_state_name(i.other_state), i.rollback_possible ? "yes" : "no", i.pending ? "yes" : "no",
           i.bootloop);
    printf("ota: updates %s, poll subscribers %d of %d\n", fw_updates_state_text(fwu_layout_ok(), fwu_rollback_ok()),
           cc_poll_subscribers(), CC_POLL_MAX_SUBS);
    return 0;
}

static int cmd_path(int argc, char **argv)
{
    char path[64];
    if (argc == 3) {
        if (!cc_link_set_fw_path(argv[2])) { printf("ota path: %s\n", tr(STR_SET_FW_PATH_BAD)); return 1; }
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

/* `ota update [corrupt-ram]`: the UI's service without the confirm. Returns at once (so `ota cancel` can follow on
   the next console line); a printer task prints each phase change, every 10 %, and the end. */
static uint32_t s_print_gen;   /* the newest note's gen when the update started */

static void printer_task(void *arg)
{
    (void)arg;
    fwu_phase_t last = FWU_IDLE;
    int last_pct = -1;
    for (;;) {
        fwu_status_t st;
        fwu_status(&st);
        int pct = fw_pct(st.done, st.total);
        if (st.phase != last || pct / 10 != last_pct / 10) {
            if (st.phase != FWU_IDLE) printf("ota update: %s %d %%\n", fwu_phase_name(st.phase), pct);
            last = st.phase;
            last_pct = pct;
        }
        if (st.phase == FWU_RESTARTING) { printf("ota update: restarting\n"); break; }
        fwu_note_t n;
        if (st.phase == FWU_IDLE && !fwu_busy()) {
            /* peek, never take: the progress screen takes the result and toasts it (fix M4) */
            if (fwu_peek_note(&n) && n.gen != s_print_gen && n.kind == FWU_NOTE_RESULT)
                printf("ota update: %s\n", tr(fw_out_text(n.out)));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelete(NULL);
}

static int cmd_update(int argc, char **argv)
{
    bool corrupt = argc == 3 && strcmp(argv[2], "corrupt-ram") == 0;
    if (argc > 3 || (argc == 3 && !corrupt)) { printf("usage: ota update [corrupt-ram]\n"); return 1; }
    fwu_check_t c;
    fwu_check(&c);
    if (c.out != FW_OUT_OK) { printf("ota update: %s\n", tr(fw_out_text(c.out))); return 1; }
    if (c.offer == FW_OFFER_UP_TO_DATE) { printf("ota update: up to date (same build)\n"); return 1; }
    fwu_debug_corrupt_ram(corrupt);
    fwu_note_t pn;
    s_print_gen = fwu_peek_note(&pn) ? pn.gen : 0;
    fw_out_t o = fwu_start();
    if (o != FW_OUT_OK) { fwu_debug_corrupt_ram(false); printf("ota update: %s\n", tr(fw_out_text(o))); return 1; }
    printf("ota update: started, %s -> %s, %u B\n", fwu_running_version(), c.desc.version, (unsigned)c.size);
    /* fix M4: the progress screen too, so the UI can't open a take-over under a running update */
    ui_async(ui_fwupdate_show, NULL);
    if (xTaskCreate(printer_task, "ota_print", 3072, NULL, 4, NULL) != pdPASS) printf("ota update: no printer task\n");
    return 0;
}

static int cmd_go_back(void)
{
    fw_out_t o = fwu_go_back();   /* returns only on a failure */
    printf("ota go-back: %s\n", tr(fw_out_text(o)));
    return 1;
}

static int cmd_test_bootloop(int argc, char **argv)
{
    int n = argc == 3 ? atoi(argv[2]) : 0;
    if (n < 1 || n > 3) { printf("usage: ota test-bootloop <1-3>\n"); return 1; }
    printf("ota test-bootloop: the next %d boot(s) panic 5 s after boot; restarting\n", n);
    fwu_debug_test_bootloop(n);
}
#endif

static int cmd_ota(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "status") == 0)) return cmd_status();
    if (argc == 2 && strcmp(argv[1], "check") == 0) return cmd_check();
    if ((argc == 2 || argc == 3) && strcmp(argv[1], "path") == 0) return cmd_path(argc, argv);
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (argc >= 2 && strcmp(argv[1], "read-test") == 0) return cmd_read_test(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "update") == 0) return cmd_update(argc, argv);
    if (argc == 2 && strcmp(argv[1], "cancel") == 0) { printf("ota cancel: %s\n", fwu_cancel() ? "asked" : tr(STR_FW_TOO_LATE)); return 0; }
    if (argc == 2 && strcmp(argv[1], "go-back") == 0) return cmd_go_back();
    if (argc >= 2 && strcmp(argv[1], "test-bootloop") == 0) return cmd_test_bootloop(argc, argv);
    printf("usage: ota [status] | ota check | ota path [<path>] | ota read-test [cancel-at <pct>] |\n"
           "       ota update [corrupt-ram] | ota cancel | ota go-back | ota test-bootloop <1-3>\n");
#else
    printf("usage: ota [status] | ota check | ota path [<path>]\n");
#endif
    return 1;
}

void fw_console_init(void)
{
    const esp_console_cmd_t c = {
        .command = "ota",
        .help = "ota [status] | check | path [<path>]: firmware updates from the Host (Plan 6)",
        .func = cmd_ota,
    };
    debug_console_register(&c);
}
