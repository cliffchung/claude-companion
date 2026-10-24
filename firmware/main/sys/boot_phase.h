#pragma once
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Plan 3 Task 3: per-boot phase timing for the check-in (ms on esp_timer, the systimer, which runs from the
 * chip reset: bootloader included). The first mark of each phase wins. The last BP_RING boots' tables are kept
 * in RTC memory (they survive deep sleep), so a chain of check-in boots can be read back in one go:
 * `power phases`. */
typedef enum {
    BP_BOOTLOADER,   /* the app's first hook (bootloader, image load and early startup done) */
    BP_APP,          /* app_main */
    BP_HOSTED,       /* esp_wifi_init returned: esp_hosted and the C6 answer */
    BP_WIFI_START,   /* WIFI_EVENT_STA_START */
    BP_JOIN,         /* esp_wifi_connect issued (the scan starts unless the join is cached) */
    BP_ASSOC,        /* WIFI_EVENT_STA_CONNECTED */
    BP_IP,           /* IP_EVENT_STA_GOT_IP */
    BP_RESOLVED,     /* the link has the Host's address */
    BP_TCP,          /* libssh: the TCP connect completed */
    BP_KEX,          /* ssh_connect returned: key exchange done */
    BP_HOSTKEY,      /* the host key check passed */
    BP_AUTH,         /* public-key auth done */
    BP_POLL,         /* the check-in's poll returned */
    BP_JUDGED,       /* the power task judged the result */
    BP_SLEEP,        /* esp_deep_sleep_start (or, staying awake by debug, where it would go down) */
    BP_N
} bp_t;

void boot_phase_init(void);          /* app_main, first line */
void boot_phase_mark(bp_t p);        /* any task; first mark wins */
uint32_t boot_phase_get(bp_t p);     /* 0 = not marked */
/* Saves this boot's table into the RTC ring (once per boot); the sleep path calls it after BP_SLEEP. */
void boot_phase_save(uint8_t boot_kind);
/* Debug chain (`power checkin hibernate ... chain <n>`): the next n check-in boots go down again with a
   test timer instead of staying awake; take() is true (and counts one) while some are left. */
void boot_phase_chain_set(uint32_t n);
bool boot_phase_chain_take(void);
/* Measurement A/B (`... exp <mask>`, RTC memory, kept until changed): a set bit turns one Task 3 change off
   (or, for BPX_PS_KEEP_MAX, keeps the old modem power save), so one build measures each change. */
#define BPX_NO_HOSTCACHE 0x02   /* look the Host up every time */
#define BPX_SLOW_WAITS   0x08   /* the check-in's waits poll every 200 ms, as before */
#define BPX_OLD_DOWN     0x10   /* the 300 + 200 ms waits on the way down, as before */
#define BPX_PS_KEEP_MAX  0x20   /* max modem power save during the check-in, as before */
#define BPX_FULL_SETTLE  0x40   /* the battery verdict needs the full 10 samples, as before */
void boot_phase_exp_set(uint32_t mask);
uint32_t boot_phase_exp(void);
void boot_phase_print(void);         /* the ring, oldest first, then this boot */
void boot_phase_clear(void);
/* ms since the timer wake, from the RTC timer (an independent check on the systimer's origin), -1 unknown */
int32_t boot_phase_rtc_since_wake_ms(void);
void boot_phase_note_sleep(uint32_t wake_after_s);   /* right before esp_deep_sleep_start */
#ifdef __cplusplus
}
#endif
