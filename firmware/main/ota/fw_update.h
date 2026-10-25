/* The firmware update service (Plan 6 Task 6, spec §7 and §12): the PSRAM download, the full verification, the
   esp_ota write to the other slot, the boot-time validity check with its 180 s rollback timer, the boot-loop guard
   and Go back. fw_update.c is the only file that calls esp_ota_* or writes an app partition (lint P6-10). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "ota/fw_rules.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { FWU_IDLE, FWU_DOWNLOADING, FWU_CHECKING, FWU_WRITING, FWU_READBACK, FWU_RESTARTING } fwu_phase_t;
typedef struct { fwu_phase_t phase; uint64_t done, total; } fwu_status_t;
typedef struct {
    fw_out_t out; int rc; uint32_t size; char sha[65]; fw_desc_t desc; fw_offer_t offer; int64_t at_ms;
} fwu_check_t;

/* Blocking (never the LVGL task): cc_link_fw_info, fw_parse_info, fw_check_header (this chip's revision), fw_offer.
   Caches the result. */
void fwu_check(fwu_check_t *out);
bool fwu_last_check(fwu_check_t *out);          /* the cached check and its age (at_ms); false if none */
/* Runs fw_gate on the live inputs; on OK starts the update task (6 KB internal) with the cached check's size and
   SHA-256 and returns FW_OUT_OK; otherwise the gate's outcome. Needs a cached OK check younger than
   FW_CHECK_TTL_MS whose offer isn't UP_TO_DATE (else FW_OUT_CHANGED: the caller re-checks). */
fw_out_t fwu_start(void);
void fwu_cancel(void);                          /* any phase before RESTARTING */
void fwu_status(fwu_status_t *out);
bool fwu_busy(void);
const char *fwu_phase_name(fwu_phase_t p);      /* "idle", "downloading", ... (console) */

/* One outcome slot (P6-17): the update's end (CANCELLED, errors) or a boot note (UPDATED, ROLLED_BACK, BOOTLOOP,
   and ROLLING_BACK: the 3 s notice before the 180 s rollback), with the version strings it needs. True once per
   outcome. */
typedef enum { FWU_NOTE_RESULT, FWU_NOTE_UPDATED, FWU_NOTE_ROLLED_BACK, FWU_NOTE_BOOTLOOP,
               FWU_NOTE_ROLLING_BACK } fwu_note_kind_t;
typedef struct { uint32_t gen; fwu_note_kind_t kind; fw_out_t out; char ver_a[33], ver_b[33]; } fwu_note_t;
bool fwu_take_note(fwu_note_t *out);
/* The same, for every kind but RESULT (Sessions' tick: a RESULT belongs to the progress screen or the console). */
bool fwu_take_boot_note(fwu_note_t *out);

void fwu_boot_init(void);                       /* spec §12.3: after NVS and settings, before Wi-Fi */
/* After cc_link_start: a PENDING_VERIFY image subscribes to the first result from the Host (cc_poll_subscribe does
   nothing before the link starts). */
void fwu_link_started(void);

/* The other slot holds a runnable image (VALID or UNDEFINED state, or no otadata entry at all: a wired flash;
   descriptor readable, project FW_PROJECT). */
bool fwu_previous(fw_desc_t *out);
/* esp_image_verify on the other slot, then esp_ota_set_boot_partition and link_down_restart. ESP_ERR_NOT_FOUND
   when not runnable; never returns on success. */
esp_err_t fwu_go_back(void);

typedef struct { const char *running_label, *other_label; int running_state, other_state; bool pending, rollback_possible; int bootloop; } fwu_info_t;
void fwu_info(fwu_info_t *out);                 /* for `ota status` and About */
const char *fwu_state_name(int state);          /* "valid", "pending", ..., "none" (no otadata entry) */
const char *fwu_running_version(void);          /* the running app descriptor's version */

/* Debug (CONFIG_COMPANION_DEBUG_COMMANDS): flip byte size / 2 of the buffer after the next download (check 6). */
void fwu_debug_corrupt_ram(bool on);
/* Debug: the next n boots panic 5 s after boot (the boot-loop guard's test); restarts now. */
void fwu_debug_test_bootloop(int n) __attribute__((noreturn));

#ifdef __cplusplus
}
#endif
