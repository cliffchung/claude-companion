#include "ota/fw_rules.h"
#include <stdio.h>
#include <string.h>

/* Little-endian reads by bytes: the buffers come off the wire, unaligned. */
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

/* A NUL inside the field's n bytes. */
static bool terminated(const uint8_t *p, size_t n) { return memchr(p, 0, n) != NULL; }

static void copy_field(char *dst, size_t cap, const uint8_t *src, size_t n)
{
    size_t l = 0;
    while (l < n && l + 1 < cap && src[l]) l++;
    memcpy(dst, src, l);
    dst[l] = 0;
}

fw_hdr_t fw_check_header(const uint8_t *b, size_t n, uint32_t image_size, uint32_t slot_size,
                         uint16_t chip_rev, fw_desc_t *out)
{
    if (!b || (n < FW_HDR_LEN && n < image_size)) return FW_HDR_SHORT;
    if (image_size < FW_MIN_SIZE) return FW_HDR_TOO_SMALL;
    if (image_size > slot_size) return FW_HDR_TOO_BIG;
    /* from here image_size >= 64 KB, so n >= FW_HDR_LEN */
    if (b[FW_OFF_MAGIC] != FW_IMG_MAGIC) return FW_HDR_MAGIC;
    uint8_t segs = b[FW_OFF_SEGMENTS];
    if (segs == 0 || segs > FW_MAX_SEGMENTS) return FW_HDR_SEGMENTS;
    if (rd16(b + FW_OFF_CHIP_ID) != FW_CHIP_ESP32P4) return FW_HDR_CHIP;
    uint16_t min_rev = rd16(b + FW_OFF_MIN_REV), max_rev = rd16(b + FW_OFF_MAX_REV);
    if (chip_rev < min_rev || chip_rev > max_rev) return FW_HDR_CHIP_REV;
    if (rd32(b + FW_OFF_DESC_MAGIC) != FW_DESC_MAGIC) return FW_HDR_DESC_MAGIC;
    if (memcmp(b + FW_OFF_PROJECT, FW_PROJECT, sizeof FW_PROJECT) != 0) return FW_HDR_PROJECT;   /* with its NUL */
    if (b[FW_OFF_VERSION] == 0 || !terminated(b + FW_OFF_VERSION, 32)) return FW_HDR_NO_VERSION;
    if (out) {
        memset(out, 0, sizeof *out);
        copy_field(out->version, sizeof out->version, b + FW_OFF_VERSION, 32);
        copy_field(out->project, sizeof out->project, b + FW_OFF_PROJECT, 32);
        copy_field(out->time, sizeof out->time, b + FW_OFF_TIME, 16);
        copy_field(out->date, sizeof out->date, b + FW_OFF_DATE, 16);
        copy_field(out->idf, sizeof out->idf, b + FW_OFF_IDF, 32);
        memcpy(out->elf_sha, b + FW_OFF_ELF_SHA, 32);
        out->chip_id = rd16(b + FW_OFF_CHIP_ID);
        out->min_rev = min_rev;
        out->max_rev = max_rev;
        out->segments = segs;
        out->hash_appended = b[FW_OFF_HASH_APPENDED] == 1;
    }
    return FW_HDR_OK;
}

const char *fw_hdr_name(fw_hdr_t h)
{
    static const char *const n[] = { "FW_HDR_OK", "FW_HDR_SHORT", "FW_HDR_MAGIC", "FW_HDR_SEGMENTS", "FW_HDR_CHIP",
                                     "FW_HDR_CHIP_REV", "FW_HDR_DESC_MAGIC", "FW_HDR_PROJECT", "FW_HDR_NO_VERSION",
                                     "FW_HDR_TOO_SMALL", "FW_HDR_TOO_BIG" };
    return (unsigned)h < sizeof n / sizeof n[0] ? n[h] : "?";
}

fw_layout_res_t fw_image_layout(const uint8_t *img, size_t len, fw_layout_t *out)
{
    fw_layout_t l = { 0 };
    if (out) *out = l;
    if (!img || len < 24) return FW_LAYOUT_TRUNCATED;
    uint8_t segs = img[FW_OFF_SEGMENTS];
    if (segs == 0 || segs > FW_MAX_SEGMENTS) return FW_LAYOUT_SEGMENTS;
    size_t off = 24;
    uint8_t ck = 0xEF;
    for (uint8_t s = 0; s < segs; s++) {
        if (len - off < 8) return FW_LAYOUT_TRUNCATED;
        uint32_t dl = rd32(img + off + 4);
        off += 8;
        if (dl > len - off) return FW_LAYOUT_TRUNCATED;   /* no overflow: off <= len here */
        for (uint32_t i = 0; i < dl; i++) ck ^= img[off + i];
        off += dl;
    }
    size_t padded = (off + 1 + 15) & ~(size_t)15;   /* the checksum is the last byte of its 16-byte block */
    if (padded > len) return FW_LAYOUT_TRUNCATED;
    l.checksum_calc = ck;
    l.checksum_stored = img[padded - 1];
    l.hash_appended = img[FW_OFF_HASH_APPENDED] == 1;
    l.hashed_len = (uint32_t)padded;
    size_t end = padded + (l.hash_appended ? 32 : 0);
    if (out) *out = l;
    if (end > len) return FW_LAYOUT_TRUNCATED;
    if (l.checksum_calc != l.checksum_stored) return FW_LAYOUT_CHECKSUM;
    if (end != len) return FW_LAYOUT_TRAILING;
    return FW_LAYOUT_OK;
}

const char *fw_layout_name(fw_layout_res_t r)
{
    static const char *const n[] = { "FW_LAYOUT_OK", "FW_LAYOUT_TRUNCATED", "FW_LAYOUT_SEGMENTS",
                                     "FW_LAYOUT_CHECKSUM", "FW_LAYOUT_TRAILING" };
    return (unsigned)r < sizeof n / sizeof n[0] ? n[r] : "?";
}

bool fw_parse_info(const uint8_t *buf, size_t len, uint32_t *size, char sha_hex[65],
                   const uint8_t **hdr, size_t *hdr_len)
{
    if (!buf) return false;
    size_t i = 0;
    uint64_t v = 0;
    while (i < len && i < 11 && buf[i] >= '0' && buf[i] <= '9') v = v * 10 + (uint64_t)(buf[i++] - '0');
    if (i == 0 || i > 10 || v > UINT32_MAX) return false;
    if (i >= len || buf[i] != ' ') return false;
    i++;
    if (len - i < 65) return false;
    for (int k = 0; k < 64; k++) {
        uint8_t c = buf[i + k];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    if (buf[i + 64] != '\n') return false;
    size_t want = v < FW_HDR_LEN ? (size_t)v : FW_HDR_LEN;
    size_t rest = len - (i + 65);
    if (rest != want) return false;
    if (size) *size = (uint32_t)v;
    if (sha_hex) { memcpy(sha_hex, buf + i, 64); sha_hex[64] = 0; }
    if (hdr) *hdr = buf + i + 65;
    if (hdr_len) *hdr_len = want;
    return true;
}

/* ---- versions ---- */

typedef struct { long n[3]; const char *pre; size_t pre_len; } semver_t;

static bool ident_char(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-'; }

static bool parse_semver(const char *s, semver_t *v)
{
    if (!s) return false;
    memset(v, 0, sizeof *v);
    if (*s == 'v' || *s == 'V') s++;
    for (int part = 0; part < 3; part++) {
        int digits = 0;
        long x = 0;
        while (*s >= '0' && *s <= '9') {
            if (++digits > 9) return false;
            x = x * 10 + (*s++ - '0');
        }
        if (digits == 0) return false;
        v->n[part] = x;
        if (*s != '.' || part == 2) break;
        s++;
    }
    if (*s == '-') {
        s++;
        v->pre = s;
        while (*s && *s != '+' && ident_char(*s)) s++;
        v->pre_len = (size_t)(s - v->pre);
        if (v->pre_len == 0) return false;
    }
    if (*s == '+') {
        s++;
        if (!*s) return false;
        while (*s && ident_char(*s)) s++;
    }
    return *s == 0;
}

fw_ver_t fw_version_cmp(const char *host, const char *running)
{
    semver_t a, b;
    if (!parse_semver(host, &a) || !parse_semver(running, &b)) return FW_VER_UNKNOWN;
    for (int i = 0; i < 3; i++)
        if (a.n[i] != b.n[i]) return a.n[i] > b.n[i] ? FW_VER_NEWER : FW_VER_OLDER;
    if (!a.pre && !b.pre) return FW_VER_SAME;
    if (!a.pre) return FW_VER_NEWER;
    if (!b.pre) return FW_VER_OLDER;
    size_t m = a.pre_len < b.pre_len ? a.pre_len : b.pre_len;
    int c = memcmp(a.pre, b.pre, m);
    if (c == 0) c = a.pre_len == b.pre_len ? 0 : (a.pre_len > b.pre_len ? 1 : -1);
    return c == 0 ? FW_VER_SAME : (c > 0 ? FW_VER_NEWER : FW_VER_OLDER);
}

fw_offer_t fw_offer(const fw_desc_t *host, const char *run_version, const uint8_t run_elf_sha[32])
{
    if (run_elf_sha && memcmp(host->elf_sha, run_elf_sha, 32) == 0) return FW_OFFER_UP_TO_DATE;
    switch (fw_version_cmp(host->version, run_version)) {
    case FW_VER_NEWER: return FW_OFFER_NEWER;
    case FW_VER_OLDER: return FW_OFFER_OLDER;
    default:           return FW_OFFER_OTHER_BUILD;
    }
}

/* ---- progress ---- */

int fw_pct(uint64_t done, uint64_t total)
{
    if (total == 0) return 0;
    if (done >= total) return 100;
    uint64_t p = done * 100 / total;
    return p > 99 ? 99 : (int)p;
}

void fw_mb_text(uint64_t done, uint64_t total, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    uint64_t d = (done + 50000) / 100000, t = (total + 50000) / 100000;   /* tenths of a MB, half up */
    snprintf(out, cap, "%llu.%llu of %llu.%llu MB", (unsigned long long)(d / 10), (unsigned long long)(d % 10),
             (unsigned long long)(t / 10), (unsigned long long)(t % 10));
}

/* ---- gates and rules ---- */

fw_gate_t fw_gate(const fw_gate_in_t *in)
{
    if (!in->layout_ok) return FW_GATE_LAYOUT;
    if (!in->rollback_ok) return FW_GATE_NO_ROLLBACK;
    if (in->updating) return FW_GATE_BUSY;
    if (in->pending) return FW_GATE_PENDING;
    if (!in->link_up) return FW_GATE_NO_LINK;
    if (in->session_open) return FW_GATE_SESSION;
    if (in->voice_active) return FW_GATE_VOICE;
    if (in->src == PR_SRC_UNKNOWN) return FW_GATE_BATTERY_UNKNOWN;
    if (in->src == PR_SRC_BATTERY && in->battery_pct < FW_BATTERY_FLOOR) return FW_GATE_BATTERY;
    if (in->psram_largest < (size_t)in->image_size + FW_PSRAM_MARGIN) return FW_GATE_MEMORY;
    return FW_GATE_OK;
}

fw_validity_t fw_validity(bool pending, bool host_ok, int64_t uptime_ms)
{
    if (!pending) return FW_V_NONE;
    if (host_ok) return FW_V_MARK_VALID;
    return uptime_ms >= FW_VALIDATE_MS ? FW_V_ROLLBACK : FW_V_WAIT;
}

int fw_bootloop_next(int count, bool crash_reset, bool was_early)
{
    if (count < 0) count = 0;
    return crash_reset && was_early ? count + 1 : 0;
}

bool fw_bootloop_trip(int count, bool other_runnable) { return count >= FW_BOOTLOOP_N && other_runnable; }

fw_boot_t fw_boot_state(bool pending, bool trusted_switch, bool target_match)
{
    if (!pending) return FW_BOOT_NONE;
    return trusted_switch && target_match ? FW_BOOT_TRUSTED : FW_BOOT_PROBATION;
}

bool fw_switch_trusted(bool entry_valid) { return entry_valid; }

bool fw_previous_ok(bool entry_valid, bool project_ok, bool marker_matches)
{
    return entry_valid && project_ok && marker_matches;
}

fw_go_back_t fw_go_back_mode(bool pending) { return pending ? FW_GB_ROLLBACK : FW_GB_SWITCH; }

bool fw_pending_claim(bool *pending)
{
    if (!pending || !*pending) return false;
    *pending = false;
    return true;
}

fw_loop_t fw_bootloop_action(int count, bool other_runnable, int fallbacks)
{
    if (count < FW_BOOTLOOP_N) return FW_LOOP_NONE;
    if (fallbacks >= FW_BOOTLOOP_MAX_FALLBACKS || !other_runnable) return FW_LOOP_GIVE_UP;
    return FW_LOOP_SWITCH;
}

bool fw_confirm_matches(const char *shown_sha, const char *now_sha)
{
    if (!shown_sha || !now_sha || strlen(shown_sha) != 64) return false;
    return strcmp(shown_sha, now_sha) == 0;
}

bool fw_attempt_rolled_back(const char *attempt, bool have_invalid, const char *invalid_version)
{
    if (!attempt || !attempt[0] || !have_invalid || !invalid_version) return false;
    return strcmp(attempt, invalid_version) == 0;
}

/* ---- outcomes ---- */

fw_out_t fw_host_rc(int rc)
{
    if (rc == 0) return FW_OUT_OK;
    if (rc == 120) return FW_OUT_NO_FILE;
    return FW_OUT_HOST_READ;
}

fw_out_t fw_hdr_out(fw_hdr_t h)
{
    switch (h) {
    case FW_HDR_OK:      return FW_OUT_OK;
    case FW_HDR_TOO_BIG: return FW_OUT_TOO_BIG;
    case FW_HDR_SHORT:   return FW_OUT_CHANGED;
    default:             return FW_OUT_NOT_APP;
    }
}

fw_out_t fw_gate_out(fw_gate_t g)
{
    switch (g) {
    case FW_GATE_OK:              return FW_OUT_OK;
    case FW_GATE_LAYOUT:          return FW_OUT_LAYOUT;
    case FW_GATE_NO_ROLLBACK:     return FW_OUT_NO_ROLLBACK;
    case FW_GATE_BUSY:            return FW_OUT_BUSY;
    case FW_GATE_PENDING:         return FW_OUT_PENDING;
    case FW_GATE_NO_LINK:         return FW_OUT_NO_LINK;
    case FW_GATE_SESSION:         return FW_OUT_SESSION;
    case FW_GATE_VOICE:           return FW_OUT_VOICE;
    case FW_GATE_BATTERY_UNKNOWN: return FW_OUT_BATTERY_UNKNOWN;
    case FW_GATE_BATTERY:         return FW_OUT_BATTERY;
    case FW_GATE_MEMORY:          return FW_OUT_NO_MEMORY;
    }
    return FW_OUT_BUSY;
}

str_id_t fw_out_text(fw_out_t o)
{
    switch (o) {
    case FW_OUT_OK:              return STR_COUNT;
    case FW_OUT_CANCELLED:       return STR_FW_CANCELLED;
    case FW_OUT_NO_LINK:         return STR_SES_NOT_CONNECTED;
    case FW_OUT_LINK_LOST:       return STR_FW_ERR_LINK;
    case FW_OUT_TIMEOUT:         return STR_FW_ERR_TIMEOUT;
    case FW_OUT_NO_FILE:         return STR_FW_ERR_NO_FILE;
    case FW_OUT_HOST_READ:       return STR_FW_ERR_HOST_READ;
    case FW_OUT_NOT_APP:         return STR_FW_ERR_NOT_APP;
    case FW_OUT_TOO_BIG:         return STR_FW_ERR_TOO_BIG;
    case FW_OUT_CORRUPT:         return STR_FW_ERR_CORRUPT;
    case FW_OUT_CHANGED:         return STR_FW_ERR_CHANGED;
    case FW_OUT_WRITE:           return STR_FW_ERR_WRITE;
    case FW_OUT_BATTERY:         return STR_FW_ERR_BATTERY;
    case FW_OUT_BATTERY_UNKNOWN: return STR_FW_ERR_BATTERY_UNKNOWN;
    case FW_OUT_SESSION:         return STR_FW_ERR_BUSY_SESSION;
    case FW_OUT_VOICE:           return STR_FW_ERR_BUSY_VOICE;
    case FW_OUT_BUSY:            return STR_FW_ERR_BUSY;
    case FW_OUT_NO_MEMORY:       return STR_FW_ERR_NO_MEMORY;
    case FW_OUT_PENDING:         return STR_FW_ERR_PENDING;
    case FW_OUT_NO_ROLLBACK:     return STR_FW_ERR_NO_ROLLBACK;
    case FW_OUT_LAYOUT:          return STR_FW_ERR_LAYOUT;
    }
    return STR_COUNT;
}
