/* The firmware update service (Plan 6 Task 6). The only file that calls esp_ota_* or writes an app partition
   (lint rule ota-outside-fw_update, P6-10). Logs carry sizes, versions, SHA-256 prefixes, durations, phases and
   exit codes only. */
#include "sdkconfig.h"
#include "ota/fw_update.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "mbedtls/sha256.h"
#include "companion/cc_link.h"
#include "settings/settings.h"
#include "sys/link_down.h"
#include "sys/link_guard.h"
#include "sys/power.h"
#include "voice/voice.h"

/* Fix C1: a stale firmware/sdkconfig keeps the options it already has, whatever sdkconfig.defaults says. A build
   without rollback (or still on the dynamic code page) must not happen silently. */
#if !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is off: a stale firmware/sdkconfig. Delete firmware/sdkconfig (it is git-ignored), then run idf.py reconfigure (README > Update over Wi-Fi)."
#endif
#if CONFIG_FATFS_CODEPAGE_DYNAMIC
#error "CONFIG_FATFS_CODEPAGE_DYNAMIC is on: a stale firmware/sdkconfig (the image would not fit the app slot's headroom). Delete firmware/sdkconfig, then run idf.py reconfigure."
#endif

static const char *TAG = "fw";

/* Task 1's byte offsets are the ESP-IDF structures' (the descriptor sits at 32 in the image: header 24 + the
   first segment's header 8). */
_Static_assert(sizeof(esp_image_header_t) == 24, "image header");
_Static_assert(sizeof(esp_image_segment_header_t) == 8, "segment header");
_Static_assert(offsetof(esp_image_header_t, magic) == FW_OFF_MAGIC, "magic");
_Static_assert(offsetof(esp_image_header_t, segment_count) == FW_OFF_SEGMENTS, "segment count");
_Static_assert(offsetof(esp_image_header_t, chip_id) == FW_OFF_CHIP_ID, "chip id");
_Static_assert(offsetof(esp_image_header_t, min_chip_rev_full) == FW_OFF_MIN_REV, "min rev");
_Static_assert(offsetof(esp_image_header_t, max_chip_rev_full) == FW_OFF_MAX_REV, "max rev");
_Static_assert(offsetof(esp_image_header_t, hash_appended) == FW_OFF_HASH_APPENDED, "hash appended");
_Static_assert(offsetof(esp_app_desc_t, magic_word) + 32 == FW_OFF_DESC_MAGIC, "desc magic");
_Static_assert(offsetof(esp_app_desc_t, version) + 32 == FW_OFF_VERSION, "version");
_Static_assert(offsetof(esp_app_desc_t, project_name) + 32 == FW_OFF_PROJECT, "project");
_Static_assert(offsetof(esp_app_desc_t, time) + 32 == FW_OFF_TIME, "time");
_Static_assert(offsetof(esp_app_desc_t, date) + 32 == FW_OFF_DATE, "date");
_Static_assert(offsetof(esp_app_desc_t, idf_ver) + 32 == FW_OFF_IDF, "idf");
_Static_assert(offsetof(esp_app_desc_t, app_elf_sha256) + 32 == FW_OFF_ELF_SHA, "elf sha");
_Static_assert(sizeof(esp_app_desc_t) + 32 == FW_HDR_LEN, "header length");
_Static_assert(FW_IMG_MAGIC == ESP_IMAGE_HEADER_MAGIC, "image magic");
_Static_assert(FW_DESC_MAGIC == ESP_APP_DESC_MAGIC_WORD, "descriptor magic");

#define CHUNK        (16 * 1024)
#define HASH_CHUNK   (64 * 1024)
#define TASK_STACK   6144
#define NVS_NS       "fw"
#define NVS_ATTEMPT  "attempt"
#define NOTICE_MS    3000     /* the rolling-back notice shows this long before the rollback */
#define SECTOR       4096

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- state (s_mux) ---- */
static fwu_check_t s_check;
static bool s_have_check;
static volatile fwu_phase_t s_phase = FWU_IDLE;
static volatile uint32_t s_done;
static uint32_t s_total;
static volatile bool s_cancel;
static volatile bool s_busy;          /* claimed by fwu_start, released by release() */
static bool s_slot_ok = true;         /* FW_SLOT_SIZE == the other slot's size (boot check) */
static bool s_rollback_ok = true;     /* the running bootloader rolls back (fix C1: a NEW state at boot says it doesn't) */
static bool s_corrupt_ram;            /* debug */
static fwu_note_t s_note;
static uint32_t s_note_gen, s_note_taken;
static bool s_pending;                /* the running image is PENDING_VERIFY; test-and-clear under s_mux (core M2) */

/* ---- the task's own (only the update task touches them) ---- */
static uint8_t *s_buf;                /* the image (size + 1), PSRAM */
static uint8_t *s_rb;                 /* the read-back block, PSRAM */
static esp_ota_handle_t s_h;
static bool s_begun;                  /* an open esp_ota handle */
static const esp_partition_t *s_dirty;   /* the other slot, written but not switched to (core M3) */
static uint32_t s_size;
static uint8_t s_sha[32];
static fw_desc_t s_desc;              /* the downloaded image's descriptor */

/* ---- boot ---- */
#define RTC_MAGIC 0x46575533u          /* "FWU3" */
typedef struct {
    uint32_t magic;
    int32_t count;          /* early crashes in a row */
    int32_t fallbacks;      /* guard switches since the last boot that lasted FW_BOOTLOOP_EARLY_MS (core M7) */
    int32_t test_left;      /* `ota test-bootloop` */
    uint8_t early;          /* this boot hasn't lasted FW_BOOTLOOP_EARLY_MS yet */
    uint8_t bootloop_note;  /* the image before this boot kept restarting and switched here */
    uint8_t trusted_switch; /* the guard or Go back switched here: mark valid at once (core I1) */
    uint8_t pad;
    uint32_t trusted_addr;  /* ...and the slot it switched to: only that slot is trusted (round 2, N2) */
} rtc_rec_t;
static RTC_NOINIT_ATTR rtc_rec_t s_rtc;
static bool s_subscribed;
static esp_timer_handle_t s_early_timer, s_valid_timer, s_test_timer;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static void arm_valid_timer(void);

static void post_note(fwu_note_kind_t kind, fw_out_t out, const char *a, const char *b)
{
    taskENTER_CRITICAL(&s_mux);
    memset(&s_note, 0, sizeof s_note);
    s_note.gen = ++s_note_gen;
    s_note.kind = kind;
    s_note.out = out;
    if (a) strlcpy(s_note.ver_a, a, sizeof s_note.ver_a);
    if (b) strlcpy(s_note.ver_b, b, sizeof s_note.ver_b);
    taskEXIT_CRITICAL(&s_mux);
}

typedef enum { TAKE_ANY, TAKE_BOOT, TAKE_RESULT } take_t;

static bool take(fwu_note_t *out, take_t which)
{
    bool got = false;
    taskENTER_CRITICAL(&s_mux);
    bool result = s_note.kind == FWU_NOTE_RESULT;
    bool fits = which == TAKE_ANY || (which == TAKE_BOOT && !result) || (which == TAKE_RESULT && result);
    if (s_note_gen != s_note_taken && fits) {
        if (out) *out = s_note;
        s_note_taken = s_note_gen;
        got = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

bool fwu_take_note(fwu_note_t *out) { return take(out, TAKE_ANY); }
bool fwu_take_boot_note(fwu_note_t *out) { return take(out, TAKE_BOOT); }
bool fwu_take_result(fwu_note_t *out) { return take(out, TAKE_RESULT); }

bool fwu_peek_note(fwu_note_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    bool have = s_note_gen != 0;
    if (have && out) *out = s_note;
    taskEXIT_CRITICAL(&s_mux);
    return have;
}

/* The one claim on a pending image (round 2, N1): mark-valid, the 180 s rollback and Go back each take it under the
   lock, and only the winner may call esp_ota_mark_*. */
static bool take_pending(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool won = fw_pending_claim(&s_pending);
    taskEXIT_CRITICAL(&s_mux);
    return won;
}

/* Posts the note only while the image is still pending, under the lock (round 2, N6: never "Going back" over
   "Updated"). */
static void post_note_if_pending(fwu_note_kind_t kind, const char *a)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_pending) {
        memset(&s_note, 0, sizeof s_note);
        s_note.gen = ++s_note_gen;
        s_note.kind = kind;
        s_note.out = FW_OUT_OK;
        if (a) strlcpy(s_note.ver_a, a, sizeof s_note.ver_a);
    }
    taskEXIT_CRITICAL(&s_mux);
}

static void put_pending(void)
{
    taskENTER_CRITICAL(&s_mux);
    s_pending = true;
    taskEXIT_CRITICAL(&s_mux);
}

bool fwu_pending(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool p = s_pending;
    taskEXIT_CRITICAL(&s_mux);
    return p;
}

bool fwu_rollback_ok(void) { return s_rollback_ok; }
bool fwu_layout_ok(void) { return s_slot_ok; }

const char *fwu_running_version(void) { return esp_app_get_description()->version; }

const char *fwu_phase_name(fwu_phase_t p)
{
    static const char *const n[] = { "idle", "downloading", "checking", "writing", "readback", "restarting" };
    return (unsigned)p < sizeof n / sizeof n[0] ? n[p] : "?";
}

const char *fwu_state_name(int st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW:            return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending";
    case ESP_OTA_IMG_VALID:          return "valid";
    case ESP_OTA_IMG_INVALID:        return "invalid";
    case ESP_OTA_IMG_ABORTED:        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:      return "undefined";
    default:                         return "none";
    }
}

static int part_state(const esp_partition_t *p)
{
    esp_ota_img_states_t st;
    if (!p || esp_ota_get_state_partition(p, &st) != ESP_OK) return -1;   /* no otadata entry: a wired flash */
    return (int)st;
}

static const esp_partition_t *other_slot(void) { return esp_ota_get_next_update_partition(NULL); }

/* ---- nvs: fw/attempt (P6-18) ---- */

static void attempt_set(const char *v) { nvs_set_str_ns(NVS_NS, NVS_ATTEMPT, v); }
static void attempt_clear(void) { nvs_erase_key_ns(NVS_NS, NVS_ATTEMPT); }

/* fw/ok_<slot> (round 2, N2): the ELF SHA-256 of the build last seen valid and running in that slot. Previous
   version (and the guard) take only a slot whose build matches its marker: a complete build that never ran has none. */
static void marker_key(const esp_partition_t *p, char *key, size_t cap) { snprintf(key, cap, "ok_%s", p->label); }

static void elf_hex(const uint8_t sha[32], char out[65])
{
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", sha[i]);
}

static void marker_write_running(void)
{
    const esp_partition_t *r = esp_ota_get_running_partition();
    if (!r) return;
    char key[24], hex[65], *old = NULL;
    marker_key(r, key, sizeof key);
    elf_hex(esp_app_get_description()->app_elf_sha256, hex);
    bool same = nvs_get_str_alloc(NVS_NS, key, &old) && old && !strcmp(old, hex);
    free(old);
    if (!same && !nvs_set_str_ns(NVS_NS, key, hex)) {
        static bool said;   /* round 3, R4: once per boot */
        if (!said) ESP_LOGW(TAG, "marker %s/%s not written: this slot won't be offered as Previous version", NVS_NS, key);
        said = true;
    }
}

static bool marker_matches(const esp_partition_t *p, const uint8_t elf_sha[32])
{
    char key[24], hex[65], *v = NULL;
    marker_key(p, key, sizeof key);
    elf_hex(elf_sha, hex);
    bool ok = nvs_get_str_alloc(NVS_NS, key, &v) && v && !strcmp(v, hex);
    free(v);
    return ok;
}

/* ---- the check ---- */

static bool hex_to_bytes(const char *hex, uint8_t out[32])
{
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

void fwu_check(fwu_check_t *out)
{
    fwu_check_t c;
    memset(&c, 0, sizeof c);
    c.rc = -1;
    uint8_t buf[CC_FW_INFO_MAX];
    size_t len = 0;
    esp_err_t e = cc_link_fw_info(buf, sizeof buf, &len, &c.rc);
    if (e == ESP_ERR_INVALID_STATE) c.out = FW_OUT_NO_LINK;
    else if (e == ESP_ERR_TIMEOUT) c.out = FW_OUT_TIMEOUT;
    else if (e == ESP_ERR_INVALID_SIZE) c.out = FW_OUT_HOST_READ;
    else if (e != ESP_OK) c.out = FW_OUT_LINK_LOST;
    else if (c.rc != 0) c.out = fw_host_rc(c.rc);
    else {
        const uint8_t *hdr = NULL;
        size_t hl = 0;
        if (!fw_parse_info(buf, len, &c.size, c.sha, &hdr, &hl)) c.out = FW_OUT_HOST_READ;
        else {
            fw_hdr_t h = fw_check_header(hdr, hl, c.size, FW_SLOT_SIZE, (uint16_t)efuse_hal_chip_revision(), &c.desc);
            c.out = fw_hdr_out(h);
            if (c.out == FW_OUT_OK) {
                const esp_app_desc_t *run = esp_app_get_description();
                c.offer = fw_offer(&c.desc, run->version, run->app_elf_sha256);
            }
        }
    }
    c.at_ms = now_ms();
    taskENTER_CRITICAL(&s_mux);
    s_check = c;
    s_have_check = true;
    taskEXIT_CRITICAL(&s_mux);
    ESP_LOGI(TAG, "check: out %d, rc %d, %u B, sha256 %.12s, version %s, offer %d", (int)c.out, c.rc,
             (unsigned)c.size, c.sha, c.desc.version, (int)c.offer);
    if (out) *out = c;
}

bool fwu_last_check(fwu_check_t *out)
{
    taskENTER_CRITICAL(&s_mux);
    bool have = s_have_check;
    if (have && out) *out = s_check;
    taskEXIT_CRITICAL(&s_mux);
    return have;
}

/* ---- the update task (spec §12.2) ---- */

typedef struct { int64_t dl, check, write, readback; } times_t;

/* P6-15: every exit goes through here. The image isn't secret, so nothing is zeroed for secrecy. A slot that was
   written but not switched to loses its first sector, so it is never offered as Previous version (core M3). */
static void release(bool restarting)
{
    if (s_begun) { esp_ota_abort(s_h); s_begun = false; }
    if (s_dirty) {
        esp_err_t e = esp_partition_erase_range(s_dirty, 0, SECTOR);
        ESP_LOGI(TAG, "%s: header erased after a failed update (%s)", s_dirty->label, esp_err_to_name(e));
        s_dirty = NULL;
    }
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    if (s_rb) { heap_caps_free(s_rb); s_rb = NULL; }
    if (!restarting) {
        power_hold(POWER_HOLD_UPDATE, false);   /* the update's own hold only: a pending check has its own (core I2) */
        taskENTER_CRITICAL(&s_mux);
        s_phase = FWU_IDLE;
        s_busy = false;
        taskEXIT_CRITICAL(&s_mux);
    }
}

static void set_phase(fwu_phase_t p, uint32_t total)
{
    taskENTER_CRITICAL(&s_mux);
    s_phase = p;
    s_done = 0;
    s_total = total;
    taskEXIT_CRITICAL(&s_mux);
}

static bool sha_eq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 32) == 0; }

/* SHA-256 of buf[0, n) in HASH_CHUNK steps, with the progress and the cancel between them. False on a cancel. */
static bool hash_buf(const uint8_t *buf, size_t n, uint8_t out[32], uint32_t base)
{
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (size_t off = 0; off < n; off += HASH_CHUNK) {
        if (s_cancel) { mbedtls_sha256_free(&c); return false; }
        size_t k = n - off > HASH_CHUNK ? HASH_CHUNK : n - off;
        mbedtls_sha256_update(&c, buf + off, k);
        s_done = base + (uint32_t)(off + k);
    }
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    return true;
}

static fw_out_t run_update(times_t *t)
{
    /* 2. the buffer */
    s_buf = heap_caps_malloc((size_t)s_size + 1, MALLOC_CAP_SPIRAM);
    if (!s_buf) return FW_OUT_NO_MEMORY;

    /* 3. the download */
    set_phase(FWU_DOWNLOADING, s_size);
    int64_t t0 = now_ms();
    size_t len = 0;
    int rc = -1;
    esp_err_t e = cc_link_fw_read(s_buf, (size_t)s_size + 1, &len, &s_done, &s_cancel, &rc,
                                  CC_FW_READ_TIMEOUT_MS(s_size));
    t->dl = now_ms() - t0;
    if (e == CC_FW_CANCELLED || s_cancel) return FW_OUT_CANCELLED;
    if (e == ESP_ERR_TIMEOUT) return FW_OUT_TIMEOUT;
    if (e == ESP_ERR_INVALID_SIZE) return FW_OUT_CHANGED;   /* the file grew */
    if (e != ESP_OK) return FW_OUT_LINK_LOST;               /* ESP_ERR_INVALID_STATE, ESP_FAIL */
    if (rc != 0) return fw_host_rc(rc);
    if (len != s_size) return FW_OUT_CHANGED;              /* check 5 */
#if CONFIG_COMPANION_DEBUG_COMMANDS
    if (s_corrupt_ram) { s_buf[s_size / 2] ^= 0x01; s_corrupt_ram = false; ESP_LOGW(TAG, "test: one byte flipped in RAM"); }
#endif

    /* 4. the checks, all before the first flash write */
    set_phase(FWU_CHECKING, s_size);
    t0 = now_ms();
    uint8_t h[32];
    if (!hash_buf(s_buf, s_size, h, 0)) return FW_OUT_CANCELLED;
    if (!sha_eq(h, s_sha)) return FW_OUT_CHANGED;           /* check 6 */
    fw_hdr_t hr = fw_check_header(s_buf, s_size, s_size, FW_SLOT_SIZE, (uint16_t)efuse_hal_chip_revision(), &s_desc);
    if (hr != FW_HDR_OK) return fw_hdr_out(hr);             /* check 7 */
    fw_layout_t l;
    if (fw_image_layout(s_buf, s_size, &l) != FW_LAYOUT_OK || !l.hash_appended) return FW_OUT_CORRUPT;   /* check 8 */
    mbedtls_sha256(s_buf, l.hashed_len, h, 0);
    if (!sha_eq(h, s_buf + l.hashed_len)) return FW_OUT_CORRUPT;
    t->check = now_ms() - t0;
    if (s_cancel) return FW_OUT_CANCELLED;

    /* 5. the write, to the other slot only */
    const esp_partition_t *next = other_slot();
    if (!next || next->size < s_size) return FW_OUT_WRITE;
    set_phase(FWU_WRITING, s_size);
    t0 = now_ms();
    ESP_LOGI(TAG, "writing %s", next->label);
    if (esp_ota_begin(next, OTA_WITH_SEQUENTIAL_WRITES, &s_h) != ESP_OK) return FW_OUT_WRITE;
    s_begun = true;
    s_dirty = next;   /* only once begin succeeded (round 2, N5): from here a failure erases its header (release) */
    for (uint32_t off = 0; off < s_size; off += CHUNK) {
        if (s_cancel) return FW_OUT_CANCELLED;              /* release() aborts the handle */
        uint32_t k = s_size - off > CHUNK ? CHUNK : s_size - off;
        if (esp_ota_write(s_h, s_buf + off, k) != ESP_OK) return FW_OUT_WRITE;
        s_done = off + k;
    }
    e = esp_ota_end(s_h);                                   /* check 9: ESP-IDF's esp_image_verify */
    s_begun = false;
    if (e != ESP_OK) { ESP_LOGW(TAG, "esp_ota_end: %s", esp_err_to_name(e)); return FW_OUT_WRITE; }
    t->write = now_ms() - t0;

    /* 6. the read-back */
    set_phase(FWU_READBACK, s_size);
    t0 = now_ms();
    s_rb = heap_caps_malloc(CHUNK, MALLOC_CAP_SPIRAM);
    if (!s_rb) return FW_OUT_NO_MEMORY;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    for (uint32_t off = 0; off < s_size; off += CHUNK) {
        if (s_cancel) { mbedtls_sha256_free(&c); return FW_OUT_CANCELLED; }
        uint32_t k = s_size - off > CHUNK ? CHUNK : s_size - off;
        if (esp_partition_read(next, off, s_rb, k) != ESP_OK) { mbedtls_sha256_free(&c); return FW_OUT_WRITE; }
        mbedtls_sha256_update(&c, s_rb, k);
        s_done = off + k;
    }
    mbedtls_sha256_finish(&c, h);
    mbedtls_sha256_free(&c);
    if (!sha_eq(h, s_sha)) return FW_OUT_WRITE;             /* check 10 */
    t->readback = now_ms() - t0;

    /* 7. the switch. From RESTARTING on a cancel is refused ("Too late to cancel", core M6). */
    bool cancelled;
    taskENTER_CRITICAL(&s_mux);
    cancelled = s_cancel;
    if (!cancelled) { s_phase = FWU_RESTARTING; s_done = s_total = s_size; }
    taskEXIT_CRITICAL(&s_mux);
    if (cancelled) return FW_OUT_CANCELLED;
    attempt_set(s_desc.version);
    e = esp_ota_set_boot_partition(next);                   /* check 11 */
    if (e != ESP_OK) { attempt_clear(); ESP_LOGW(TAG, "set boot: %s", esp_err_to_name(e)); return FW_OUT_WRITE; }
    s_dirty = NULL;   /* switched: keep it */
    return FW_OUT_OK;
}

static void update_task(void *arg)
{
    (void)arg;
    times_t t = { 0 };
    char from[33];
    strlcpy(from, fwu_running_version(), sizeof from);
    fw_out_t o = run_update(&t);
    ESP_LOGI(TAG, "%s -> %s, %u B, download %.1f s, check %.1f s, write %.1f s, readback %.1f s, %s", from,
             s_desc.version[0] ? s_desc.version : "?", (unsigned)s_size, t.dl / 1000.0, t.check / 1000.0,
             t.write / 1000.0, t.readback / 1000.0, o == FW_OUT_OK ? "restarting" : tr(fw_out_text(o)));
    if (o != FW_OUT_OK) {
        post_note(FWU_NOTE_RESULT, o, NULL, NULL);   /* before IDLE: whoever sees IDLE finds the note */
        release(false);
        vTaskDelete(NULL);
        return;
    }
    release(true);   /* the buffer freed; the hold and busy stay until the restart */
    ESP_LOGI(TAG, "heapmin %u B", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelay(pdMS_TO_TICKS(1000));   /* the progress screen shows "Restarting…" */
    link_down_restart("firmware update");
}

/* The live gate inputs. need_link and image_size: Install's; Go back needs neither. */
static void gate_inputs(fw_gate_in_t *g, uint32_t image_size, bool for_install)
{
    voice_status_t vs;
    voice_status(&vs);
    int pct = -1;
    bool chg = false;
    memset(g, 0, sizeof *g);
    g->layout_ok = s_slot_ok;
    g->rollback_ok = s_rollback_ok;
    g->pending = for_install && fwu_pending();
    g->link_up = for_install ? cc_link_connected() : true;
    g->session_open = power_hold_is_on(POWER_HOLD_TAKEOVER) || power_hold_is_on(POWER_HOLD_SSH);
    g->voice_active = vs.phase != VOICE_IDLE;
    g->updating = s_busy;
    g->src = power_source(&pct, &chg);
    g->battery_pct = pct;
    g->psram_largest = for_install ? heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) : SIZE_MAX;
    g->image_size = for_install ? image_size : 0;
}

fw_out_t fwu_start_expect(const char *expect_sha)
{
    fwu_check_t c;
    bool have = fwu_last_check(&c);
    if (s_busy) return FW_OUT_BUSY;
    fw_gate_in_t g;
    gate_inputs(&g, have ? c.size : 0, true);
    if (!g.layout_ok) return FW_OUT_LAYOUT;
    if (!g.rollback_ok) return FW_OUT_NO_ROLLBACK;
    if (g.pending) return FW_OUT_PENDING;   /* core I2: an unvalidated image can't be updated (esp_ota_begin refuses) */
    if (!have || c.out != FW_OUT_OK || c.offer == FW_OFFER_UP_TO_DATE || now_ms() - c.at_ms >= FW_CHECK_TTL_MS)
        return FW_OUT_CHANGED;
    if (expect_sha && !fw_confirm_matches(expect_sha, c.sha)) return FW_OUT_CHANGED;   /* fix I3 */
    fw_out_t o = fw_gate_out(fw_gate(&g));
    if (o != FW_OUT_OK) return o;
    taskENTER_CRITICAL(&s_mux);
    if (s_busy) o = FW_OUT_BUSY;
    else { s_busy = true; s_cancel = false; s_phase = FWU_DOWNLOADING; s_done = 0; s_total = c.size; }
    taskEXIT_CRITICAL(&s_mux);
    if (o != FW_OUT_OK) return o;
    s_size = c.size;
    s_dirty = NULL;
    if (!hex_to_bytes(c.sha, s_sha)) { release(false); return FW_OUT_HOST_READ; }
    memset(&s_desc, 0, sizeof s_desc);
    power_hold(POWER_HOLD_UPDATE, true);
    if (xTaskCreate(update_task, "fw_update", TASK_STACK, NULL, 5, NULL) != pdPASS) {
        release(false);
        return FW_OUT_NO_MEMORY;
    }
    return FW_OUT_OK;
}

fw_out_t fwu_start(void) { return fwu_start_expect(NULL); }

bool fwu_cancel(void)
{
    taskENTER_CRITICAL(&s_mux);
    bool ok = s_busy && s_phase != FWU_RESTARTING;
    if (ok) s_cancel = true;
    taskEXIT_CRITICAL(&s_mux);
    return ok;
}

void fwu_status(fwu_status_t *out)
{
    if (!out) return;
    taskENTER_CRITICAL(&s_mux);
    out->phase = s_phase;
    out->done = s_done;
    out->total = s_total;
    taskEXIT_CRITICAL(&s_mux);
}

bool fwu_busy(void) { return s_busy; }

void fwu_debug_corrupt_ram(bool on) { s_corrupt_ram = on; }

/* ---- the other slot ---- */

bool fwu_previous(fw_desc_t *out)
{
    const esp_partition_t *o = other_slot();
    if (!o) return false;
    esp_app_desc_t d;
    bool have = esp_ota_get_partition_description(o, &d) == ESP_OK;
    bool project = have && strncmp(d.project_name, FW_PROJECT, sizeof d.project_name) == 0;
    if (!fw_previous_ok(part_state(o) == ESP_OTA_IMG_VALID, project, project && marker_matches(o, d.app_elf_sha256)))
        return false;   /* round 2, N2: a VALID entry and a marker for this build; never an entry-less slot */
    if (out) {
        memset(out, 0, sizeof *out);
        strlcpy(out->version, d.version, sizeof out->version);
        strlcpy(out->project, d.project_name, sizeof out->project);
        strlcpy(out->date, d.date, sizeof out->date);
        strlcpy(out->time, d.time, sizeof out->time);
        strlcpy(out->idf, d.idf_ver, sizeof out->idf);
        memcpy(out->elf_sha, d.app_elf_sha256, 32);
    }
    return true;
}

/* A switch to a slot whose entry is VALID: the trusted flag, bound to that slot's address, makes the next boot mark
   it valid at once instead of putting it on probation (core I1; round 2, N2: only a VALID target). Set before
   esp_ota_set_boot_partition, cleared on a failure. Refused while the running entry is PENDING_VERIFY in otadata
   (round 3, R1): then only the rollback may act, and a switch would hand it the switch's target to invalidate. */
static esp_err_t switch_to_other(void)
{
    const esp_partition_t *o = other_slot();
    if (part_state(esp_ota_get_running_partition()) == ESP_OTA_IMG_PENDING_VERIFY) return ESP_ERR_INVALID_STATE;
    if (!o || !fwu_previous(NULL)) return ESP_ERR_NOT_FOUND;
    esp_partition_pos_t pos = { .offset = o->address, .size = o->size };
    esp_image_metadata_t md;
    esp_err_t e = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &md);
    if (e != ESP_OK) { ESP_LOGW(TAG, "switch: %s doesn't verify (%s)", o->label, esp_err_to_name(e)); return e; }
    bool trusted = fw_switch_trusted(part_state(o) == ESP_OTA_IMG_VALID);
    s_rtc.trusted_switch = trusted;
    s_rtc.trusted_addr = trusted ? o->address : 0;
    e = esp_ota_set_boot_partition(o);
    if (e != ESP_OK) {
        s_rtc.trusted_switch = 0;
        s_rtc.trusted_addr = 0;
        ESP_LOGW(TAG, "switch: set boot %s: %s", o->label, esp_err_to_name(e));
    }
    return e;
}

fw_out_t fwu_go_back(void)
{
    fw_gate_in_t g;
    gate_inputs(&g, 0, false);
    g.rollback_ok = true;   /* going back needs no rollback support: the target was good */
    g.layout_ok = true;
    fw_out_t o = fw_gate_out(fw_gate(&g));
    if (o != FW_OUT_OK) return o;
    /* round 3, R1: the mode comes from otadata, not s_pending (the 180 s rollback clears that while it still waits
       in lg_close to act). Pending in otadata: claim it; a lost claim is BUSY, never a switch. */
    bool otadata_pending = part_state(esp_ota_get_running_partition()) == ESP_OTA_IMG_PENDING_VERIFY;
    bool won = otadata_pending && take_pending();
    fw_go_back_t mode = fw_go_back_decide(otadata_pending, won);
    if (mode == FW_GB_BUSY) return FW_OUT_GOING_BACK;
    if (mode == FW_GB_ROLLBACK) {
        /* round 2, N1: a pending image goes back by the rollback itself. The previous slot keeps its VALID entry;
           no switch, so nothing can invalidate it. It needs no marker (R6): the bootloader's check is the test. */
        if (!esp_ota_check_rollback_is_possible()) { put_pending(); return FW_OUT_WRITE; }
        if (s_valid_timer) esp_timer_stop(s_valid_timer);
        ESP_LOGI(TAG, "go back while pending: rolling back");
        lg_token_t t = lg_close(false, LG_CLOSE_MS);
        esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   /* returns only on a failure */
        ESP_LOGE(TAG, "go back: rollback failed (%s)", esp_err_to_name(e));
        lg_reopen(t);
        put_pending();
        arm_valid_timer();   /* round 3, R2: the 180 s path still runs (and ends the hold if it fails too) */
        return FW_OUT_WRITE;
    }
    if (switch_to_other() != ESP_OK) return FW_OUT_WRITE;
    ESP_LOGI(TAG, "go back: restarting into the other slot");
    link_down_restart("firmware go back");
}

void fwu_info(fwu_info_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    const esp_partition_t *r = esp_ota_get_running_partition(), *o = other_slot();
    out->running_label = r ? r->label : "?";
    out->other_label = o ? o->label : "?";
    out->running_state = part_state(r);
    out->other_state = part_state(o);
    out->pending = fwu_pending();
    out->rollback_possible = esp_ota_check_rollback_is_possible();
    out->bootloop = s_rtc.magic == RTC_MAGIC ? (int)s_rtc.count : 0;
}

/* ---- boot (spec §12.3) ---- */

static void early_cb(void *arg)
{
    (void)arg;
    s_rtc.early = 0;     /* FW_BOOTLOOP_EARLY_MS up: this boot didn't crash early */
    s_rtc.count = 0;
    s_rtc.fallbacks = 0;
}

static void test_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "test-bootloop: panicking now");
    abort();
}

static void make_timer(esp_timer_cb_t cb, const char *name, esp_timer_handle_t *out)
{
    const esp_timer_create_args_t a = { .callback = cb, .name = name };
    if (esp_timer_create(&a, out) != ESP_OK) *out = NULL;
}

static void valid_timer_cb(void *arg);

/* The pending check's timer: at the notice (FW_VALIDATE_MS - NOTICE_MS of uptime) or, past it, at FW_VALIDATE_MS, or
   at once past that. */
static void arm_valid_timer(void)
{
    if (!s_valid_timer) make_timer(valid_timer_cb, "fw_valid", &s_valid_timer);
    if (!s_valid_timer) return;
    int64_t up = now_ms();
    int64_t at = up < FW_VALIDATE_MS - NOTICE_MS ? FW_VALIDATE_MS - NOTICE_MS : FW_VALIDATE_MS;
    int64_t in = at - up;
    esp_timer_stop(s_valid_timer);
    esp_timer_start_once(s_valid_timer, (uint64_t)(in > 0 ? in : 1) * 1000);
}

/* A crash that the guard counts: a panic, a watchdog, a CPU lockup (core M4). A brownout or a power-on isn't. */
static bool crash_reset(esp_reset_reason_t why)
{
    return why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
           why == ESP_RST_CPU_LOCKUP;
}

void fwu_boot_early(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = other_slot();
    s_slot_ok = next && next->size == FW_SLOT_SIZE;
    esp_reset_reason_t why = esp_reset_reason();
    if (s_rtc.magic != RTC_MAGIC || why == ESP_RST_POWERON) {
        memset(&s_rtc, 0, sizeof s_rtc);
        s_rtc.magic = RTC_MAGIC;
    }

    /* a trusted switch (the guard or Go back): the image was good before the switch, so no probation (core I1) */
    bool pending = part_state(running) == ESP_OTA_IMG_PENDING_VERIFY;
    fw_boot_t b = fw_boot_state(pending, s_rtc.trusted_switch != 0, running && running->address == s_rtc.trusted_addr);
    s_rtc.trusted_switch = 0;
    s_rtc.trusted_addr = 0;
    if (b == FW_BOOT_TRUSTED) {
        esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "%s (%s): switched to by the boot-loop guard or Go back, marked valid (%s)", fwu_running_version(),
                 running->label, esp_err_to_name(e));
    }

    /* the boot-loop guard (§7.6.3, core M7) */
    s_rtc.count = fw_bootloop_next(s_rtc.count, crash_reset(why), s_rtc.early != 0);
    s_rtc.early = 1;
    if (s_rtc.bootloop_note) {
        s_rtc.bootloop_note = 0;
        post_note(FWU_NOTE_BOOTLOOP, FW_OUT_OK, fwu_running_version(), NULL);
    }
    if (s_rtc.count > 0) ESP_LOGW(TAG, "boot-loop guard: %d early crash(es) in a row", (int)s_rtc.count);
    /* fwu_previous reads the fw/ok_<slot> marker from NVS: open it only when the guard may act (nvs_flash_init is
       safe to repeat; settings_init calls it again; a failure leaves no target, so the guard gives up) */
    bool runnable = false;
    if (s_rtc.count >= FW_BOOTLOOP_N && s_slot_ok && nvs_flash_init() == ESP_OK) runnable = fwu_previous(NULL);
    fw_loop_t a = fw_bootloop_action(s_rtc.count, runnable, s_rtc.fallbacks);
    if (a == FW_LOOP_SWITCH) {
        ESP_LOGE(TAG, "boot-loop guard: %d early crashes: switching to %s", (int)s_rtc.count, next->label);
        if (switch_to_other() == ESP_OK) {
            s_rtc.count = 0;
            s_rtc.early = 0;
            s_rtc.fallbacks++;
            s_rtc.bootloop_note = 1;
            esp_restart();   /* the first thing in app_main: no SSH session exists yet */
        }
        a = FW_LOOP_GIVE_UP_ALONE;
    }
    if (a == FW_LOOP_GIVE_UP_BOTH || a == FW_LOOP_GIVE_UP_ALONE) {
        ESP_LOGE(TAG, "boot-loop guard: %d early crashes, %s (fallbacks %d): flash a good build over USB",
                 (int)s_rtc.count, a == FW_LOOP_GIVE_UP_BOTH ? "both versions" : "no earlier version",
                 (int)s_rtc.fallbacks);
        post_note(a == FW_LOOP_GIVE_UP_BOTH ? FWU_NOTE_BOOTLOOP_BOTH : FWU_NOTE_BOOTLOOP_ALONE, FW_OUT_OK, NULL, NULL);
    }
    make_timer(early_cb, "fw_early", &s_early_timer);
    if (s_early_timer) esp_timer_start_once(s_early_timer, (uint64_t)FW_BOOTLOOP_EARLY_MS * 1000);

    /* the test hook (`ota test-bootloop`) */
    if (s_rtc.test_left > 0) {
        s_rtc.test_left--;
        ESP_LOGW(TAG, "test-bootloop: panic in 5 s (%d more after this)", (int)s_rtc.test_left);
        make_timer(test_cb, "fw_test", &s_test_timer);
        if (s_test_timer) esp_timer_start_once(s_test_timer, 5000 * 1000);
    }
}

/* The first result from the Host: mark valid (the poll task). Test-and-clear of s_pending, so a rollback at the
   same moment can't also run (core M2). */
static void mark_valid_now(void)
{
#if CONFIG_COMPANION_OTA_TEST_NO_VALIDATE
    static bool said;
    if (!said) { ESP_LOGW(TAG, "test build: not marking valid"); said = true; }
    return;
#else
    if (!take_pending()) return;
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK) { ESP_LOGW(TAG, "mark valid: %s", esp_err_to_name(e)); put_pending(); return; }
    if (s_valid_timer) esp_timer_stop(s_valid_timer);
    attempt_clear();
    marker_write_running();   /* round 2, N2: this build ran and reached the Host */
    power_hold(POWER_HOLD_FW_PENDING, false);   /* the pending check's hold only (core I2) */
    ESP_LOGI(TAG, "marked valid at %lld ms: %s", (long long)now_ms(), fwu_running_version());
    post_note(FWU_NOTE_UPDATED, FW_OUT_OK, fwu_running_version(), NULL);
#endif
}

static void on_host_result(const cc_session_t *s, int n, void *ctx)
{
    (void)s; (void)n; (void)ctx;
    if (fwu_pending()) mark_valid_now();
}

/* P6-7 at 180 s, on a task of its own (it closes SSH first, which blocks): back to the previous slot (core M1). */
static void rollback_task(void *arg)
{
    (void)arg;
    if (take_pending()) {
        ESP_LOGW(TAG, "not valid after %d s: rolling back", FW_VALIDATE_MS / 1000);
        lg_token_t t = lg_close(false, LG_CLOSE_MS);   /* C1: SSH closes before the restart */
        esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();   /* returns only on a failure */
        ESP_LOGE(TAG, "rollback failed (%s): keeping %s for now", esp_err_to_name(e), fwu_running_version());
        lg_reopen(t);    /* round 2, N3: the link comes back, so a later Host result can still mark it valid */
        put_pending();   /* the next reset still rolls back */
        power_hold(POWER_HOLD_FW_PENDING, false);
        post_note(FWU_NOTE_ROLLBACK_FAILED, FW_OUT_OK, NULL, NULL);
    }
    vTaskDelete(NULL);
}

/* P6-7: at FW_VALIDATE_MS - NOTICE_MS the notice, then at FW_VALIDATE_MS the rollback, unless valid by then. */
static void valid_timer_cb(void *arg)
{
    (void)arg;
    if (!fwu_pending()) return;
    if (fw_validity(true, false, now_ms()) == FW_V_ROLLBACK) {
        if (xTaskCreate(rollback_task, "fw_rollback", 4096, NULL, 5, NULL) != pdPASS) {
            ESP_LOGE(TAG, "rollback: no task; the next reset goes back");
            power_hold(POWER_HOLD_FW_PENDING, false);
        }
        return;
    }
    const esp_partition_t *o = other_slot();
    esp_app_desc_t d;
    const char *prev = o && esp_ota_get_partition_description(o, &d) == ESP_OK ? d.version : "";
    post_note_if_pending(FWU_NOTE_ROLLING_BACK, prev);   /* round 2, N6 */
    int64_t left = FW_VALIDATE_MS - now_ms();
    esp_timer_start_once(s_valid_timer, (uint64_t)(left > 0 ? left : 1) * 1000);
}

void fwu_boot_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = other_slot();
    if (!s_slot_ok)
        ESP_LOGE(TAG, "the other app slot is %u B, not FW_SLOT_SIZE %u: updates disabled",
                 next ? (unsigned)next->size : 0u, (unsigned)FW_SLOT_SIZE);
#if CONFIG_COMPANION_OTA_TEST_NO_VALIDATE
    ESP_LOGW(TAG, "test build: never marks itself valid");
#endif
    int st = part_state(running);
    /* fix C1: a bootloader with rollback turns a NEW image into PENDING_VERIFY. NEW at boot means this bootloader
       has none (a partial wired flash): it can't go back, so updates stay off. */
    if (st == ESP_OTA_IMG_NEW) {
        s_rollback_ok = false;
        ESP_LOGE(TAG, "rollback off: the bootloader booted %s without checking it. Updates are off until a wired "
                      "idf.py flash of this build (README > Update over Wi-Fi)", running->label);
        esp_ota_mark_app_valid_cancel_rollback();
        attempt_clear();
        return;
    }

    if (st == ESP_OTA_IMG_VALID) marker_write_running();   /* round 2, N2 (a trusted switch, a wired flash) */

    /* a new image waiting for its check (a trusted switch was marked valid in fwu_boot_early) */
    if (st == ESP_OTA_IMG_PENDING_VERIFY) {
        put_pending();
        power_hold(POWER_HOLD_FW_PENDING, true);   /* stays Active: no sleep may reset the check */
        arm_valid_timer();
        ESP_LOGI(TAG, "%s (%s) pending verify: valid after the first result from the Host, else back in %d s",
                 fwu_running_version(), running->label, FW_VALIDATE_MS / 1000);
        return;
    }

    /* the old image after a rollback: only when the invalid slot holds the attempted version (core M5) */
    char *att = NULL;
    if (nvs_get_str_alloc(NVS_NS, NVS_ATTEMPT, &att) && att) {
        const esp_partition_t *inv = esp_ota_get_last_invalid_partition();
        esp_app_desc_t d;
        bool have = inv && esp_ota_get_partition_description(inv, &d) == ESP_OK;
        if (fw_attempt_rolled_back(att, have, have ? d.version : NULL)) {
            ESP_LOGW(TAG, "the update to %s didn't start properly: back on %s", att, fwu_running_version());
            post_note(FWU_NOTE_ROLLED_BACK, FW_OUT_OK, att, fwu_running_version());
        } else {
            ESP_LOGI(TAG, "fw/attempt %s with no matching rollback (a cut before the switch): cleared", att);
        }
        free(att);
        attempt_clear();
    }
}

void fwu_link_started(void)
{
    if (fwu_pending() && !s_subscribed) {
        s_subscribed = cc_poll_subscribe(on_host_result, NULL);
        if (!s_subscribed) ESP_LOGE(TAG, "no poll subscriber slot: this image can't mark itself valid");
    }
}

void fwu_debug_test_bootloop(int n)
{
    if (s_rtc.magic != RTC_MAGIC) { memset(&s_rtc, 0, sizeof s_rtc); s_rtc.magic = RTC_MAGIC; }
    s_rtc.test_left = n;
    link_down_restart("ota test-bootloop");
}
