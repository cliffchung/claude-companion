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
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "mbedtls/sha256.h"
#include "companion/cc_link.h"
#include "settings/settings.h"
#include "sys/link_down.h"
#include "sys/power.h"
#include "voice/voice.h"

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
static bool s_corrupt_ram;            /* debug */
static fwu_note_t s_note;
static uint32_t s_note_gen, s_note_taken;

/* ---- the task's own (only the update task touches them) ---- */
static uint8_t *s_buf;                /* the image (size + 1), PSRAM */
static uint8_t *s_rb;                 /* the read-back block, PSRAM */
static esp_ota_handle_t s_h;
static bool s_begun;
static uint32_t s_size;
static uint8_t s_sha[32];
static fw_desc_t s_desc;              /* the downloaded image's descriptor */

/* ---- boot ---- */
#define RTC_MAGIC 0x46575531u          /* "FWU1" */
typedef struct { uint32_t magic; int32_t count; uint8_t early, bootloop_note, pad[2]; int32_t test_left; } rtc_rec_t;
static RTC_NOINIT_ATTR rtc_rec_t s_rtc;
static bool s_pending;                /* the running image is PENDING_VERIFY */
static bool s_subscribed;
static esp_timer_handle_t s_early_timer, s_valid_timer, s_test_timer;

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

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

static bool take(fwu_note_t *out, bool boot_only)
{
    bool got = false;
    taskENTER_CRITICAL(&s_mux);
    if (s_note_gen != s_note_taken && !(boot_only && s_note.kind == FWU_NOTE_RESULT)) {
        if (out) *out = s_note;
        s_note_taken = s_note_gen;
        got = true;
    }
    taskEXIT_CRITICAL(&s_mux);
    return got;
}

bool fwu_take_note(fwu_note_t *out) { return take(out, false); }
bool fwu_take_boot_note(fwu_note_t *out) { return take(out, true); }

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

/* P6-15: every exit goes through here. The image isn't secret, so nothing is zeroed for secrecy. */
static void release(bool restarting)
{
    if (s_begun) { esp_ota_abort(s_h); s_begun = false; }
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    if (s_rb) { heap_caps_free(s_rb); s_rb = NULL; }
    if (!restarting) {
        power_hold(POWER_HOLD_UPDATE, false);
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
    if (s_cancel) return FW_OUT_CANCELLED;

    /* 7. the switch */
    attempt_set(s_desc.version);
    e = esp_ota_set_boot_partition(next);                   /* check 11 */
    if (e != ESP_OK) { attempt_clear(); ESP_LOGW(TAG, "set boot: %s", esp_err_to_name(e)); return FW_OUT_WRITE; }
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
    set_phase(FWU_RESTARTING, s_size);
    release(true);   /* the buffer freed; the hold and busy stay until the restart */
    ESP_LOGI(TAG, "heapmin %u B", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    vTaskDelay(pdMS_TO_TICKS(1000));   /* the progress screen shows "Restarting…" */
    link_down_restart("firmware update");
}

fw_out_t fwu_start(void)
{
    if (!s_slot_ok) return FW_OUT_WRITE;
    fwu_check_t c;
    bool have = fwu_last_check(&c);
    taskENTER_CRITICAL(&s_mux);
    bool busy = s_busy;
    taskEXIT_CRITICAL(&s_mux);
    if (busy) return FW_OUT_BUSY;
    if (!have || c.out != FW_OUT_OK || c.offer == FW_OFFER_UP_TO_DATE || now_ms() - c.at_ms >= FW_CHECK_TTL_MS)
        return FW_OUT_CHANGED;
    voice_status_t vs;
    voice_status(&vs);
    int pct = -1;
    bool chg = false;
    fw_gate_in_t g = {
        .link_up = cc_link_connected(),
        .session_open = power_hold_is_on(POWER_HOLD_TAKEOVER) || power_hold_is_on(POWER_HOLD_SSH),
        .voice_active = vs.phase != VOICE_IDLE,
        .updating = false,
        .psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        .image_size = c.size,
    };
    g.src = power_source(&pct, &chg);
    g.battery_pct = pct;
    fw_out_t o = fw_gate_out(fw_gate(&g));
    if (o != FW_OUT_OK) return o;
    taskENTER_CRITICAL(&s_mux);
    if (s_busy) o = FW_OUT_BUSY;
    else { s_busy = true; s_cancel = false; s_phase = FWU_DOWNLOADING; s_done = 0; s_total = c.size; }
    taskEXIT_CRITICAL(&s_mux);
    if (o != FW_OUT_OK) return o;
    s_size = c.size;
    if (!hex_to_bytes(c.sha, s_sha)) { release(false); return FW_OUT_HOST_READ; }
    memset(&s_desc, 0, sizeof s_desc);
    power_hold(POWER_HOLD_UPDATE, true);
    if (xTaskCreate(update_task, "fw_update", TASK_STACK, NULL, 5, NULL) != pdPASS) {
        release(false);
        return FW_OUT_NO_MEMORY;
    }
    return FW_OUT_OK;
}

void fwu_cancel(void)
{
    taskENTER_CRITICAL(&s_mux);
    if (s_busy && s_phase != FWU_RESTARTING) s_cancel = true;
    taskEXIT_CRITICAL(&s_mux);
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
    int st = part_state(o);
    if (!(st == ESP_OTA_IMG_VALID || st == ESP_OTA_IMG_UNDEFINED || st < 0)) return false;
    esp_app_desc_t d;
    if (esp_ota_get_partition_description(o, &d) != ESP_OK) return false;
    if (strncmp(d.project_name, FW_PROJECT, sizeof d.project_name) != 0) return false;
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

static esp_err_t switch_to_other(void)
{
    const esp_partition_t *o = other_slot();
    if (!o || !fwu_previous(NULL)) return ESP_ERR_NOT_FOUND;
    esp_partition_pos_t pos = { .offset = o->address, .size = o->size };
    esp_image_metadata_t md;
    esp_err_t e = esp_image_verify(ESP_IMAGE_VERIFY, &pos, &md);
    if (e != ESP_OK) { ESP_LOGW(TAG, "go back: %s doesn't verify (%s)", o->label, esp_err_to_name(e)); return e; }
    e = esp_ota_set_boot_partition(o);
    if (e != ESP_OK) ESP_LOGW(TAG, "go back: set boot %s: %s", o->label, esp_err_to_name(e));
    return e;
}

esp_err_t fwu_go_back(void)
{
    if (s_busy) return ESP_ERR_INVALID_STATE;
    esp_err_t e = switch_to_other();
    if (e != ESP_OK) return e;
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
    out->pending = s_pending;
    out->rollback_possible = esp_ota_check_rollback_is_possible();
    out->bootloop = s_rtc.magic == RTC_MAGIC ? (int)s_rtc.count : 0;
}

/* ---- boot (spec §12.3) ---- */

static void early_cb(void *arg)
{
    (void)arg;
    s_rtc.early = 0;     /* 60 s up: this boot didn't crash early */
    s_rtc.count = 0;
}

static void test_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "test-bootloop: panicking now");
    abort();
}

static void valid_timer_cb(void *arg);

static void mark_valid_now(void)
{
#if CONFIG_COMPANION_OTA_TEST_NO_VALIDATE
    static bool said;
    if (!said) { ESP_LOGW(TAG, "test build: not marking valid"); said = true; }
    return;
#else
    if (!s_pending) return;
    if (fw_validity(true, true, now_ms()) != FW_V_MARK_VALID) return;
    esp_err_t e = esp_ota_mark_app_valid_cancel_rollback();
    if (e != ESP_OK) { ESP_LOGW(TAG, "mark valid: %s", esp_err_to_name(e)); return; }
    s_pending = false;
    if (s_valid_timer) esp_timer_stop(s_valid_timer);
    attempt_clear();
    power_hold(POWER_HOLD_UPDATE, false);
    ESP_LOGI(TAG, "marked valid at %lld ms: %s", (long long)now_ms(), fwu_running_version());
    post_note(FWU_NOTE_UPDATED, FW_OUT_OK, fwu_running_version(), NULL);
#endif
}

/* The poll task, after each successful result from the Host (a poll, a watch block or heartbeat). */
static void on_host_result(const cc_session_t *s, int n, void *ctx)
{
    (void)s; (void)n; (void)ctx;
    if (s_pending) mark_valid_now();
}

/* P6-7: at FW_VALIDATE_MS - NOTICE_MS the notice, then at FW_VALIDATE_MS the rollback, unless valid by then. */
static void valid_timer_cb(void *arg)
{
    (void)arg;
    if (!s_pending) return;
    fw_validity_t v = fw_validity(true, false, now_ms());
    if (v == FW_V_ROLLBACK) {
        ESP_LOGW(TAG, "not valid after %d s: rolling back", FW_VALIDATE_MS / 1000);
        esp_ota_mark_app_invalid_rollback_and_reboot();   /* restarts into the previous slot */
        return;
    }
    /* the notice; the rollback when FW_VALIDATE_MS is reached */
    const esp_partition_t *o = other_slot();
    esp_app_desc_t d;
    const char *prev = o && esp_ota_get_partition_description(o, &d) == ESP_OK ? d.version : "";
    post_note(FWU_NOTE_ROLLING_BACK, FW_OUT_OK, prev, NULL);
    int64_t left = FW_VALIDATE_MS - now_ms();
    esp_timer_start_once(s_valid_timer, (uint64_t)(left > 0 ? left : 1) * 1000);
}

static void make_timer(esp_timer_cb_t cb, const char *name, esp_timer_handle_t *out)
{
    const esp_timer_create_args_t a = { .callback = cb, .name = name };
    if (esp_timer_create(&a, out) != ESP_OK) *out = NULL;
}

void fwu_boot_init(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *next = other_slot();
    if (!next || next->size != FW_SLOT_SIZE) {
        ESP_LOGE(TAG, "the other app slot is %u B, not FW_SLOT_SIZE %u: updates disabled",
                 next ? (unsigned)next->size : 0u, (unsigned)FW_SLOT_SIZE);
        s_slot_ok = false;
    }

    /* 1. the boot-loop guard (§7.6.3) */
    esp_reset_reason_t why = esp_reset_reason();
    if (s_rtc.magic != RTC_MAGIC || why == ESP_RST_POWERON) {
        memset(&s_rtc, 0, sizeof s_rtc);
        s_rtc.magic = RTC_MAGIC;
    }
    bool crash = why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT;
    s_rtc.count = fw_bootloop_next(s_rtc.count, crash, s_rtc.early != 0);
    s_rtc.early = 1;
    if (s_rtc.bootloop_note) {   /* the image before this boot kept restarting and switched here */
        s_rtc.bootloop_note = 0;
        post_note(FWU_NOTE_BOOTLOOP, FW_OUT_OK, fwu_running_version(), NULL);
    }
    if (s_rtc.count > 0) ESP_LOGW(TAG, "boot-loop guard: %d early crash(es) in a row", (int)s_rtc.count);
    if (fw_bootloop_trip(s_rtc.count, s_slot_ok && fwu_previous(NULL))) {
        ESP_LOGE(TAG, "boot-loop guard: %d early crashes: switching to %s", (int)s_rtc.count, next->label);
        if (switch_to_other() == ESP_OK) {
            s_rtc.count = 0;
            s_rtc.early = 0;
            s_rtc.bootloop_note = 1;
            /* before link_down_init and Wi-Fi: no SSH session exists yet, so a plain restart */
            esp_restart();
        }
    }
    make_timer(early_cb, "fw_early", &s_early_timer);
    if (s_early_timer) esp_timer_start_once(s_early_timer, (uint64_t)FW_BOOTLOOP_EARLY_MS * 1000);

    /* 2. the test hook (`ota test-bootloop`) */
    if (s_rtc.test_left > 0) {
        s_rtc.test_left--;
        ESP_LOGW(TAG, "test-bootloop: panic in 5 s (%d more after this)", (int)s_rtc.test_left);
        make_timer(test_cb, "fw_test", &s_test_timer);
        if (s_test_timer) esp_timer_start_once(s_test_timer, 5000 * 1000);
    }

#if CONFIG_COMPANION_OTA_TEST_NO_VALIDATE
    ESP_LOGW(TAG, "test build: never marks itself valid");
#endif

    /* 3. a new image waiting for its check */
    if (part_state(running) == ESP_OTA_IMG_PENDING_VERIFY) {
        s_pending = true;
        power_hold(POWER_HOLD_UPDATE, true);   /* stays Active: no sleep may reset the check */
        make_timer(valid_timer_cb, "fw_valid", &s_valid_timer);
        int64_t at = FW_VALIDATE_MS - NOTICE_MS - now_ms();
        if (s_valid_timer) esp_timer_start_once(s_valid_timer, (uint64_t)(at > 0 ? at : 1) * 1000);
        ESP_LOGI(TAG, "%s (%s) pending verify: valid after the first result from the Host, else back in %d s",
                 fwu_running_version(), running->label, FW_VALIDATE_MS / 1000);
        return;
    }

    /* 4. the old image after a rollback */
    char *att = NULL;
    if (nvs_get_str_alloc(NVS_NS, NVS_ATTEMPT, &att) && att) {
        if (esp_ota_get_last_invalid_partition()) {
            ESP_LOGW(TAG, "the update to %s didn't start properly: back on %s", att, fwu_running_version());
            post_note(FWU_NOTE_ROLLED_BACK, FW_OUT_OK, att, fwu_running_version());
        }
        free(att);
        attempt_clear();
    }
}

void fwu_link_started(void)
{
    if (s_pending && !s_subscribed) {
        cc_poll_subscribe(on_host_result, NULL);
        s_subscribed = true;
    }
}

void fwu_debug_test_bootloop(int n)
{
    if (s_rtc.magic != RTC_MAGIC) { memset(&s_rtc, 0, sizeof s_rtc); s_rtc.magic = RTC_MAGIC; }
    s_rtc.test_left = n;
    link_down_restart("ota test-bootloop");
}
