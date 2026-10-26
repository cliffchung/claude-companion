/* Pure firmware-update logic (Plan 6 Task 1): no ESP-IDF or LVGL headers, host-tested (test/host/test_fw_rules.c).
   The image checks of spec §7.1, the version compare and offer of §7.2, the progress maths of §8.4, the gates of
   §7.4, the validity and boot-loop rules of §7.3 and §7.6, and the outcome -> copy mapping. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "i18n/i18n.h"
#include "sys/power_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_HDR_LEN          288          /* image header 24 + first segment header 8 + app descriptor 256 */
#define FW_IMG_MAGIC        0xE9
#define FW_DESC_MAGIC       0xABCD5432u
#define FW_CHIP_ESP32P4     0x0012
#define FW_PROJECT          "claude_companion"
#define FW_SLOT_SIZE        0x7F0000u    /* == app0/app1 size in partitions.csv (test_check_image_size.py asserts it) */
#define FW_MIN_SIZE         (64u * 1024u)
#define FW_MAX_SEGMENTS     16
#define FW_BATTERY_FLOOR    30
#define FW_PSRAM_MARGIN     (1024u * 1024u)
#define FW_VALIDATE_MS      180000
#define FW_BOOTLOOP_N       3
#define FW_BOOTLOOP_EARLY_MS 60000
#define FW_CHECK_TTL_MS     60000
#define FW_BOOTLOOP_MAX_FALLBACKS 1   /* guard switches before it gives up (core M7: no ping-pong) */

/* Byte offsets in the first FW_HDR_LEN bytes of an image (fw_update.c pins them to esp_image_header_t and
   esp_app_desc_t with _Static_assert). */
#define FW_OFF_MAGIC        0
#define FW_OFF_SEGMENTS     1
#define FW_OFF_CHIP_ID      12
#define FW_OFF_MIN_REV      15
#define FW_OFF_MAX_REV      17
#define FW_OFF_HASH_APPENDED 23
#define FW_OFF_DESC         32
#define FW_OFF_DESC_MAGIC   (FW_OFF_DESC + 0)
#define FW_OFF_VERSION      (FW_OFF_DESC + 16)
#define FW_OFF_PROJECT      (FW_OFF_DESC + 48)
#define FW_OFF_TIME         (FW_OFF_DESC + 80)
#define FW_OFF_DATE         (FW_OFF_DESC + 96)
#define FW_OFF_IDF          (FW_OFF_DESC + 112)
#define FW_OFF_ELF_SHA      (FW_OFF_DESC + 144)

typedef struct {
    char version[33], project[33], date[17], time[17], idf[33];
    uint8_t elf_sha[32];
    uint16_t chip_id, min_rev, max_rev;          /* full revisions: 100 = v1.0 */
    uint8_t segments;
    bool hash_appended;
} fw_desc_t;

typedef enum { FW_HDR_OK, FW_HDR_SHORT, FW_HDR_MAGIC, FW_HDR_SEGMENTS, FW_HDR_CHIP, FW_HDR_CHIP_REV,
               FW_HDR_DESC_MAGIC, FW_HDR_PROJECT, FW_HDR_NO_VERSION, FW_HDR_TOO_SMALL, FW_HDR_TOO_BIG } fw_hdr_t;
/* b: the first n bytes of an image of image_size bytes. Checks in this order: SHORT (n < FW_HDR_LEN and
   n < image_size), TOO_SMALL, TOO_BIG (> slot_size), MAGIC, SEGMENTS (0 or > FW_MAX_SEGMENTS), CHIP,
   CHIP_REV (chip_rev outside min..max), DESC_MAGIC, PROJECT (!= FW_PROJECT), NO_VERSION (empty or not
   NUL-terminated within 32). out (may be NULL) is filled on OK, NUL-terminating every string. */
fw_hdr_t fw_check_header(const uint8_t *b, size_t n, uint32_t image_size, uint32_t slot_size,
                         uint16_t chip_rev, fw_desc_t *out);
const char *fw_hdr_name(fw_hdr_t h);             /* "FW_HDR_OK" ... (console and fw_print) */

typedef struct { uint32_t hashed_len; uint8_t checksum_calc, checksum_stored; bool hash_appended; } fw_layout_t;
typedef enum { FW_LAYOUT_OK, FW_LAYOUT_TRUNCATED, FW_LAYOUT_SEGMENTS, FW_LAYOUT_CHECKSUM,
               FW_LAYOUT_TRAILING } fw_layout_res_t;
/* Walks header (24), each segment (8 + data_len, inside len), pads to the 16-byte boundary where the XOR
   checksum (seed 0xEF, over every segment's data) sits, then the 32-byte SHA-256 when hash_appended. The
   walk must end exactly at len (TRAILING otherwise). The caller hashes img[0, hashed_len) and compares it
   with img[hashed_len, +32). */
fw_layout_res_t fw_image_layout(const uint8_t *img, size_t len, fw_layout_t *out);
const char *fw_layout_name(fw_layout_res_t r);   /* "FW_LAYOUT_OK" ... */

/* "<size> <64 lowercase hex>\n" then min(288, size) bytes. False for anything else. */
bool fw_parse_info(const uint8_t *buf, size_t len, uint32_t *size, char sha_hex[65],
                   const uint8_t **hdr, size_t *hdr_len);

typedef enum { FW_VER_OLDER = -1, FW_VER_SAME = 0, FW_VER_NEWER = 1, FW_VER_UNKNOWN = 2 } fw_ver_t;
/* host relative to running. "v"/"V" prefix allowed; MAJOR[.MINOR[.PATCH]][-pre][+build]; missing parts are
   0; the build part is ignored; a pre-release is lower than the same release; pre-releases compare as
   strings. Anything else (a bare git hash) is UNKNOWN. */
fw_ver_t fw_version_cmp(const char *host, const char *running);

typedef enum { FW_OFFER_UP_TO_DATE, FW_OFFER_NEWER, FW_OFFER_OLDER, FW_OFFER_OTHER_BUILD } fw_offer_t;
/* same ELF SHA-256 -> UP_TO_DATE; else NEWER / OLDER by fw_version_cmp; SAME or UNKNOWN -> OTHER_BUILD */
fw_offer_t fw_offer(const fw_desc_t *host, const char *run_version, const uint8_t run_elf_sha[32]);

int  fw_pct(uint64_t done, uint64_t total);       /* floor; 0 for total 0; 99 until done >= total, then 100 */
/* "3.2 of 6.4 MB" (MB = 10^6, one decimal, rounded half up); "0.0 of 0.0 MB" for zeros. */
void fw_mb_text(uint64_t done, uint64_t total, char *out, size_t cap);

typedef struct {
    bool layout_ok;     /* the slots are this plan's (round 2, N8: its own copy) */
    bool rollback_ok;   /* the bootloader can roll back (fix C1); false refuses every update */
    bool pending;       /* the running image still waits to be marked valid (core I2) */
    bool link_up, session_open, voice_active, updating;
    pr_src_t src; int battery_pct;
    size_t psram_largest; uint32_t image_size;
} fw_gate_in_t;
typedef enum { FW_GATE_OK, FW_GATE_LAYOUT, FW_GATE_NO_ROLLBACK, FW_GATE_BUSY, FW_GATE_PENDING, FW_GATE_NO_LINK, FW_GATE_SESSION, FW_GATE_VOICE,
               FW_GATE_BATTERY_UNKNOWN, FW_GATE_BATTERY, FW_GATE_MEMORY } fw_gate_t;
fw_gate_t fw_gate(const fw_gate_in_t *in);       /* first failing in the enum's order */

typedef enum { FW_V_NONE, FW_V_WAIT, FW_V_MARK_VALID, FW_V_ROLLBACK } fw_validity_t;
/* not pending -> NONE; host_ok -> MARK_VALID; uptime >= FW_VALIDATE_MS -> ROLLBACK; else WAIT */
fw_validity_t fw_validity(bool pending, bool host_ok, int64_t uptime_ms);

/* Boot (core I1): a PENDING_VERIFY image the guard or Go back switched to (trusted_switch: it was VALID or a wired
   flash before the switch) is marked valid at once, with no probation; any other pending image gets the probation. */
typedef enum { FW_BOOT_NONE, FW_BOOT_PROBATION, FW_BOOT_TRUSTED } fw_boot_t;
/* target_match (round 2, N2): the running slot is the one the flag was set for (its address is stored with it). */
fw_boot_t fw_boot_state(bool pending, bool trusted_switch, bool target_match);
/* round 2, N2: a switch is trusted (no probation) only when its target's otadata entry is VALID: an entry-less slot
   may be a complete build that never ran (esp_ota_begin erases the entry; a power cut before the switch keeps it). */
bool fw_switch_trusted(bool entry_valid);
/* round 2, N2: the other slot is offered as Previous version (and is the guard's target) only with a VALID entry, a
   claude_companion descriptor, and the fw/ok_<slot> marker (written when that build was valid and running) naming
   its ELF SHA-256. */
bool fw_previous_ok(bool entry_valid, bool project_ok, bool marker_matches);
/* Go back (round 2 N1, round 3 R1). otadata_pending: the running entry is PENDING_VERIFY in otadata (the truth, not
   the in-memory flag, which the 180 s rollback clears while it still waits to act); claim_won: this caller took the
   pending claim. Pending and won: ROLLBACK (Go back is the rollback); pending and lost: BUSY (the rollback or a
   mark-valid already acts; never a switch, which would let the rollback invalidate the switch's target); not
   pending: SWITCH. */
typedef enum { FW_GB_SWITCH, FW_GB_ROLLBACK, FW_GB_BUSY } fw_go_back_t;
fw_go_back_t fw_go_back_decide(bool otadata_pending, bool claim_won);
/* `ota status`'s updates word (round 3 R3): "on", "off (old partition table)" or "off (rollback off)". */
const char *fw_updates_state_text(bool layout_ok, bool rollback_ok);
/* round 2, N1: the claim on a pending image (the rollback task, Go back, mark-valid): test-and-clear, so exactly one
   claimant wins. The firmware calls it under its lock. */
bool fw_pending_claim(bool *pending);
/* The boot-loop guard's step (core M7): under FW_BOOTLOOP_N early crashes NONE; after FW_BOOTLOOP_MAX_FALLBACKS
   switches without a stable boot in between GIVE_UP_BOTH (both versions crash: stop); with no runnable other slot
   GIVE_UP_ALONE (nothing to go back to, round 3 R5: a different notice); else SWITCH. fallbacks resets when a boot
   lasts FW_BOOTLOOP_EARLY_MS. */
typedef enum { FW_LOOP_NONE, FW_LOOP_SWITCH, FW_LOOP_GIVE_UP_BOTH, FW_LOOP_GIVE_UP_ALONE } fw_loop_t;
fw_loop_t fw_bootloop_action(int count, bool other_runnable, int fallbacks);
/* fix I3: the staged image is still the one the confirm showed (same 64-hex SHA-256; false for NULL or empty). */
bool fw_confirm_matches(const char *shown_sha, const char *now_sha);
/* core M5: the old image reports "didn't start properly" only when an invalid slot exists and holds the attempted
   version (a power cut between writing fw/attempt and the switch leaves the key with no rollback). */
bool fw_attempt_rolled_back(const char *attempt, bool have_invalid, const char *invalid_version);
/* The count after this boot: crash_reset && was_early -> count + 1, else 0. */
int  fw_bootloop_next(int count, bool crash_reset, bool was_early);
bool fw_bootloop_trip(int count, bool other_runnable);   /* count >= FW_BOOTLOOP_N && other_runnable */

typedef enum { FW_OUT_OK, FW_OUT_CANCELLED, FW_OUT_NO_LINK, FW_OUT_LINK_LOST, FW_OUT_TIMEOUT, FW_OUT_NO_FILE,
               FW_OUT_HOST_READ, FW_OUT_NOT_APP, FW_OUT_TOO_BIG, FW_OUT_CORRUPT, FW_OUT_CHANGED, FW_OUT_WRITE,
               FW_OUT_BATTERY, FW_OUT_BATTERY_UNKNOWN, FW_OUT_SESSION, FW_OUT_VOICE, FW_OUT_BUSY,
               FW_OUT_NO_MEMORY, FW_OUT_PENDING, FW_OUT_NO_ROLLBACK, FW_OUT_LAYOUT,
               FW_OUT_GOING_BACK } fw_out_t;
fw_out_t fw_host_rc(int rc);                     /* 0 OK; 120 NO_FILE; 121, 122, 126, 127 and others HOST_READ */
fw_out_t fw_hdr_out(fw_hdr_t h);                 /* TOO_BIG -> TOO_BIG; SHORT -> CHANGED; the rest -> NOT_APP */
fw_out_t fw_gate_out(fw_gate_t g);
str_id_t fw_out_text(fw_out_t o);                /* STR_COUNT for OK */

#ifdef __cplusplus
}
#endif
