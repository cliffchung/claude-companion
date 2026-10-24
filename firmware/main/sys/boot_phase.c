#include "boot_phase.h"
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_private/esp_clk.h"
#include "esp_private/startup_internal.h"
#include "freertos/FreeRTOS.h"

static const char *const k_name[BP_N] = { "boot", "app", "hosted", "wstart", "join", "assoc", "ip", "resolv",
                                          "tcp", "kex", "hkey", "auth", "poll", "judged", "sleep" };

static uint32_t s_mark[BP_N];
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_saved;
static int32_t s_rtc_since_wake = -1;

#define BP_RING 8
#define BP_MAGIC 0x42505431u
typedef struct {
    uint32_t magic;
    uint32_t n, head, chain, exp;
    uint64_t wake_rtc_us;          /* the RTC time the armed timer fires (set on the way down) */
    struct { uint32_t kind, exp; int32_t rtc_wake; uint32_t mark[BP_N]; } r[BP_RING];
    uint32_t check;
} bp_rtc_t;
static RTC_NOINIT_ATTR bp_rtc_t s_rtc;

static uint32_t rtc_check(void)
{
    const uint32_t *w = (const uint32_t *)&s_rtc;
    uint32_t c = 0x9e3779b9u;
    for (size_t i = 0; i < offsetof(bp_rtc_t, check) / 4; i++) c = (c ^ w[i]) * 16777619u;
    return c;
}
static bool rtc_valid(void) { return s_rtc.magic == BP_MAGIC && s_rtc.check == rtc_check() && s_rtc.n <= BP_RING && s_rtc.head < BP_RING; }
static void rtc_seal(void) { s_rtc.magic = BP_MAGIC; s_rtc.check = rtc_check(); }
static void rtc_ensure(void)
{
    if (rtc_valid()) return;
    memset(&s_rtc, 0, sizeof(s_rtc));
    rtc_seal();
}

static uint32_t now_ms(void) { uint32_t t = (uint32_t)(esp_timer_get_time() / 1000); return t ? t : 1; }

/* the app's first code that can run: the bootloader has loaded and handed over */
ESP_SYSTEM_INIT_FN(boot_phase_first, CORE, BIT(0), 102)   /* right after esp_timer_init_nonos (101) */
{
    s_mark[BP_BOOTLOADER] = now_ms();
    return ESP_OK;
}

extern void (*ssh_companion_tcp_connected_hook)(void);
static void tcp_hook(void) { boot_phase_mark(BP_TCP); }

void boot_phase_init(void)
{
    boot_phase_mark(BP_APP);
    ssh_companion_tcp_connected_hook = tcp_hook;
    rtc_ensure();
    if (s_rtc.wake_rtc_us) {
        int64_t d = (int64_t)esp_clk_rtc_time() - (int64_t)s_rtc.wake_rtc_us;
        /* only a plausible timer wake (not a reset long after, or a power-on) */
        if (d > 0 && d < 60000000LL) s_rtc_since_wake = (int32_t)(d / 1000);
        s_rtc.wake_rtc_us = 0;
        rtc_seal();
    }
}

void boot_phase_mark(bp_t p)
{
    if (p >= BP_N) return;
    uint32_t t = now_ms();
    taskENTER_CRITICAL(&s_mux);
    if (!s_mark[p]) s_mark[p] = t;
    taskEXIT_CRITICAL(&s_mux);
}

uint32_t boot_phase_get(bp_t p) { return p < BP_N ? s_mark[p] : 0; }
int32_t boot_phase_rtc_since_wake_ms(void) { return s_rtc_since_wake; }

void boot_phase_save(uint8_t boot_kind)
{
    if (s_saved) return;
    s_saved = true;
    rtc_ensure();
    uint32_t i = s_rtc.head;
    s_rtc.r[i].kind = boot_kind;
    s_rtc.r[i].exp = s_rtc.exp;
    s_rtc.r[i].rtc_wake = s_rtc_since_wake;
    memcpy(s_rtc.r[i].mark, s_mark, sizeof(s_mark));
    s_rtc.head = (i + 1) % BP_RING;
    if (s_rtc.n < BP_RING) s_rtc.n++;
    rtc_seal();
}

void boot_phase_note_sleep(uint32_t wake_after_s)
{
    rtc_ensure();
    s_rtc.wake_rtc_us = esp_clk_rtc_time() + (uint64_t)wake_after_s * 1000000ULL;
    rtc_seal();
}

void boot_phase_chain_set(uint32_t n)
{
    rtc_ensure();
    s_rtc.chain = n;
    rtc_seal();
}

bool boot_phase_chain_take(void)
{
    rtc_ensure();
    if (!s_rtc.chain) return false;
    s_rtc.chain--;
    rtc_seal();
    return true;
}

void boot_phase_clear(void)
{
    uint32_t exp = rtc_valid() ? s_rtc.exp : 0;
    memset(&s_rtc, 0, sizeof(s_rtc));
    s_rtc.exp = exp;
    rtc_seal();
}

void boot_phase_exp_set(uint32_t mask)
{
    rtc_ensure();
    s_rtc.exp = mask;
    rtc_seal();
}
uint32_t boot_phase_exp(void) { return rtc_valid() ? s_rtc.exp : 0; }

static void print_row(const char *label, uint32_t kind, uint32_t exp, int32_t rtc_wake, const uint32_t *m)
{
    printf("%-5s k%u x%02x", label, (unsigned)kind, (unsigned)exp);
    for (int p = 0; p < BP_N; p++) {
        if (m[p]) printf(" %s=%u", k_name[p], (unsigned)m[p]);
        else printf(" %s=-", k_name[p]);
    }
    if (rtc_wake >= 0) printf(" rtcwake=%d", (int)rtc_wake);
    printf("\n");
}

void boot_phase_print(void)
{
    rtc_ensure();
    printf("phases (ms from reset; chain left %u):\n", (unsigned)s_rtc.chain);
    for (uint32_t i = 0; i < s_rtc.n; i++) {
        uint32_t k = (s_rtc.head + BP_RING - s_rtc.n + i) % BP_RING;
        char lab[8];
        snprintf(lab, sizeof(lab), "#%u", (unsigned)i);
        print_row(lab, s_rtc.r[k].kind, s_rtc.r[k].exp, s_rtc.r[k].rtc_wake, s_rtc.r[k].mark);
    }
    print_row("now", 0, s_rtc.exp, s_rtc_since_wake, s_mark);
}
