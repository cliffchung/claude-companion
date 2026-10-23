#include "checkin.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings/settings.h"
#include "net/wifi_mgr.h"
#include "net/tailscale.h"
#include "companion/cc_link.h"
#include "input/keyboard.h"
#include "sys/sysinfo.h"
#include "sys/power.h"
#include "sys/hibernate.h"

static const char *TAG = "checkin";

#define JOB_STACK 6144
#define WIFI_WAIT_MS 20000          /* step 2 */
#define LINK_WAIT_MS 15000          /* step 4 */
#define POLL_MS 15000               /* step 5 */
#define JUDGED_WAIT_MS 2000         /* the power task judges the result (needs-you wake or not) */
#define HIB_BUDGET_MS 180000        /* the whole Hibernate check-in boot */
#define HIB_WATCHDOG_MS 200000      /* a job stuck past its budget: hibernate anyway */
#define HIB_WIFI_WAIT_MS 60000      /* a cold boot: the C6 boots and joins */
#define HIB_LINK_WAIT_MS 30000
#define HIB_TEST_WAKE_S 60
#define CHK_N 8
#define JOB_RETRIES 20              /* failed task creates retried each loop (about 10 s), then the fallback */
/* M1: the longest a Radio-off check-in can take: Wi-Fi 20 + tunnel (8 + 60 + 8) + link 15 + a stale-key
   rebind (8 + 60 + 8) + link 15 + poll 15 + judge 2 s = 219 s, plus the connect wait in tailscale_checkin
   (up to 60 s) = 279 s: the light keeps a result this much longer than the period */
#define CHECKIN_WORST_MIN 5
#define HOLD_STALE_MS (60 * 60 * 1000LL)   /* a hold with no result for an hour: down, light off (I1) */

static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }

/* ---- records: `power checkins`, NVS ns "power" key "chk" ------------------ */

typedef enum { P_HANDSHAKE, P_REBIND, P_FAILED, P_HIB_HANDSHAKE, P_HIB_REBIND, P_HIB_FAILED, P_HIB_LOW } ck_path_t;
static const char *const k_path[] = { "handshake", "rebind", "failed", "hib-handshake", "hib-rebind", "hib-failed", "hib-low" };
#define K_PATH_N 7
typedef struct { uint32_t epoch, ms, mwh_x1000; uint8_t path, tag, needs, pad; } ck_rec_t;
typedef struct { uint8_t ver, n, head, pad; ck_rec_t r[CHK_N]; } ck_log_t;
#define CHK_VER 1
static ck_log_t s_log;              /* under s_mux */

/* ---- state ----------------------------------------------------------------- */

typedef enum { JOB_NONE, JOB_PARK, JOB_UNPARK, JOB_CHECKIN, JOB_HIB, JOB_DOWN } job_t;
static const char *const k_job[] = { "none", "park", "unpark", "check-in", "hibernate check-in", "hibernate" };

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_running;     /* a job task exists */
static volatile job_t s_job;
static volatile bool s_parked;      /* the radio is parked by us */
static volatile bool s_woke_parked;
static volatile uint8_t s_tag;      /* the running check-in's log tag */
static volatile int64_t s_mw_sum;   /* its 1 Hz mW samples */
static volatile uint32_t s_mw_n;
static volatile uint32_t s_seq;     /* check-ins finished */

/* power task only */
static power_state_t s_prev = PS_ACTIVE;
static int64_t s_next_ms;           /* the next check-in, 0 = none */
static int s_period;                /* the period that set s_next_ms */
static int s_deep_k;                /* check-ins done since entering Deep idle */
static bool s_force, s_force_rebind;
static volatile bool s_bench_park;
static int64_t s_job_started_ms;
static bool s_hib_pending;          /* a check-in boot: start JOB_HIB on the first step */
static volatile bool s_quiet;       /* checkin_hib_quiet */
static volatile bool s_hold;        /* checkin_hib_hold */
static uint32_t s_hold_gen;
static int64_t s_hold_gen_ms;       /* when the hold last saw a new result */
static bool s_watchdog_fired;
static int s_watchdog_tries, s_hib_tries;
static volatile bool s_hib_settled;
static bool s_hold_down;            /* the hold is over: JOB_DOWN until it starts */
static int s_down_tries; /* the check-in boot reached a state that isn't "going down" (hold, debug, gave up) */

/* the job's arguments */
static bool s_arg_rebind;
static power_state_t s_arg_state;

bool checkin_radio_parked(void) { return s_parked; }
bool checkin_busy(void) { return s_running; }
bool checkin_woke_parked(void) { return s_woke_parked; }
int checkin_period_min(void) { return (s_parked || (s_running && s_job == JOB_CHECKIN)) && s_next_ms ? s_period : 0; }
int checkin_light_period_min(void)
{
    int p = checkin_period_min();
    return p > 0 ? p + CHECKIN_WORST_MIN : 0;
}
int checkin_next_s(void)
{
    if (!s_parked || !s_next_ms) return -1;
    int64_t left = s_next_ms - now_ms();
    return left > 0 ? (int)((left + 999) / 1000) : 0;
}
uint8_t checkin_log_tag(void) { return s_running ? s_tag : 0; }
static bool s_inited;
/* before checkin_init (the keyboard probe asks for the light early in the boot) the boot kind decides */
bool checkin_hib_quiet(void) { return s_inited ? s_quiet : hibernate_boot_kind() == POWER_BOOT_HIB_CHECKIN; }
bool checkin_hib_hold(void) { return s_hold; }
uint32_t checkin_seq(void) { return s_seq; }
bool checkin_ran_since(uint32_t seq) { return s_seq != seq && !s_running; }
void checkin_bench_park(bool on) { s_bench_park = on; }

void checkin_on_sample(void)
{
    if (!s_running || !s_tag) return;
    power_info_t p = sysinfo_power();
    if (!p.valid) return;
    float mw = p.bus_v * (p.current_a < 0 ? -p.current_a : p.current_a) * 1000.0f;
    taskENTER_CRITICAL(&s_mux);
    s_mw_sum += (int64_t)(mw + 0.5f);
    s_mw_n++;
    taskEXIT_CRITICAL(&s_mux);
}

static void sums_reset(uint8_t tag)
{
    taskENTER_CRITICAL(&s_mux);
    s_mw_sum = 0;
    s_mw_n = 0;
    s_tag = tag;
    taskEXIT_CRITICAL(&s_mux);
}

/* 1 Hz samples: mWh = sum(mW) / 3600 */
static uint32_t mwh_x1000(void)
{
    taskENTER_CRITICAL(&s_mux);
    int64_t sum = s_mw_sum;
    taskEXIT_CRITICAL(&s_mux);
    return (uint32_t)(sum * 1000 / 3600);
}

static void log_load(void)
{
    void *blob = NULL;
    size_t len = 0;
    memset(&s_log, 0, sizeof(s_log));
    if (nvs_get_blob_alloc("power", "chk", &blob, &len)) {
        if (len == sizeof(s_log) && ((ck_log_t *)blob)->ver == CHK_VER) memcpy(&s_log, blob, len);
        free(blob);
    }
    s_log.ver = CHK_VER;
    if (s_log.n > CHK_N || s_log.head >= CHK_N) { s_log.n = 0; s_log.head = 0; }
}

/* Job task only (the NVS write needs an internal stack). */
static void record(ck_path_t path, uint32_t ms, bool needs)
{
    ck_rec_t r = { .epoch = (uint32_t)time(NULL), .ms = ms, .mwh_x1000 = mwh_x1000(), .path = (uint8_t)path,
                   .tag = s_tag, .needs = needs };
    ck_log_t copy;
    taskENTER_CRITICAL(&s_mux);
    s_log.r[s_log.head] = r;
    s_log.head = (uint8_t)((s_log.head + 1) % CHK_N);
    if (s_log.n < CHK_N) s_log.n++;
    copy = s_log;
    taskEXIT_CRITICAL(&s_mux);
    nvs_set_blob_ns("power", "chk", &copy, sizeof(copy));
    ESP_LOGI(TAG, "check-in 0x%02x: %s in %u ms, %u.%03u mWh (%u samples)%s", r.tag, k_path[path], (unsigned)ms,
             (unsigned)(r.mwh_x1000 / 1000), (unsigned)(r.mwh_x1000 % 1000), (unsigned)s_mw_n, needs ? ", needs you" : "");
    s_seq++;
}

void checkin_print(void)
{
    ck_log_t l;
    taskENTER_CRITICAL(&s_mux);
    l = s_log;
    taskEXIT_CRITICAL(&s_mux);
    if (!l.n) { printf("checkins: none\n"); return; }
    for (int i = 0; i < l.n; i++) {
        const ck_rec_t *r = &l.r[(l.head - l.n + i + CHK_N) % CHK_N];
        char when[24] = "-";
        time_t t = (time_t)r->epoch;
        struct tm tm;
        if (t > 1600000000 && localtime_r(&t, &tm)) strftime(when, sizeof(when), "%m-%d %H:%M:%S", &tm);
        printf("%s 0x%02x %s %u ms %u.%03u mWh%s\n", when, r->tag, r->path < K_PATH_N ? k_path[r->path] : "?", (unsigned)r->ms,
               (unsigned)(r->mwh_x1000 / 1000), (unsigned)(r->mwh_x1000 % 1000), r->needs ? " needs-you" : "");
    }
}

/* ---- what the radio should be ---------------------------------------------- */

static bool want_parked(power_state_t st)
{
    if (st == PS_RADIO_OFF) return true;
    if (st == PS_DEEP_IDLE) return power_cfg(PCFG_RADIO_OFF) || s_bench_park;
    return false;
}

static bool wait_for(bool (*cond)(void), int ms)
{
    for (int64_t end = now_ms() + ms; !cond(); vTaskDelay(pdMS_TO_TICKS(200)))
        if (now_ms() >= end) return false;
    return true;
}

static bool wifi_up(void) { return wifi_mgr_is_connected(); }
static bool link_up(void) { return cc_link_connected(); }
static bool link_down(void) { return !cc_link_connected(); }
static bool judged(void) { return power_state() < PS_OFF || power_results_judged(); }

/* Steps 4-5: SSH, then one plain poll. A handshake path counts only once SSH works: a stale keypair can
   report the peer up, so a failed connect after a handshake escalates to a rebind. */
static ts_checkin_t link_and_poll(const char *mac, ts_checkin_t ts, int link_ms, bool *poll_ok)
{
    *poll_ok = false;
    cc_link_park(false);
    bool up = wait_for(link_up, link_ms);
    if (!up && ts == TS_CHECKIN_HANDSHAKE) {
        ESP_LOGW(TAG, "handshake but no SSH in %d ms: rebinding", link_ms);
        ts = tailscale_checkin(mac, true, NULL);
        if (ts != TS_CHECKIN_FAILED) {
            cc_link_on_wifi(true);   /* retry the connect now, not after the backoff */
            up = wait_for(link_up, link_ms);
        }
    }
    if (!up) return TS_CHECKIN_FAILED;
    *poll_ok = cc_link_poll_now(POLL_MS) == ESP_OK;
    return ts;
}

static void park_now(void)
{
    cc_link_park(true);
    wait_for(link_down, 2000);   /* the SSH close goes out before the radio stops */
    bool ok = wifi_mgr_park();
    s_parked = true;
    ESP_LOGI(TAG, "radio parked: park mode %s%s", wifi_mgr_park_mode(), ok ? "" : " (the park failed: radio may be on)");
}

/* Steps 1-5 of a check-in (also the input wake's reconnect). Returns the path. */
static ts_checkin_t bring_up(bool rebind, bool *poll_ok, int64_t t0)
{
    char mac[128];
    *poll_ok = false;
    tailscale_set_fast_resume(true);
    s_parked = false;
    wifi_mgr_unpark();
    if (!wait_for(wifi_up, WIFI_WAIT_MS)) {
        ESP_LOGW(TAG, "no Wi-Fi after %d ms", WIFI_WAIT_MS);
        return TS_CHECKIN_FAILED;
    }
    if (!cc_link_mac_address(mac, sizeof(mac))) return TS_CHECKIN_FAILED;
    ts_checkin_t ts = tailscale_checkin(mac, rebind, NULL);
    ESP_LOGI(TAG, "tunnel: %s at %lld ms", tailscale_checkin_name(ts), (long long)(now_ms() - t0));
    if (ts == TS_CHECKIN_FAILED) return ts;
    return link_and_poll(mac, ts, LINK_WAIT_MS, poll_ok);
}

/* ---- jobs (the checkin task) ------------------------------------------------- */

static void job_checkin(void)
{
    int64_t t0 = now_ms();
    power_state_t st = s_arg_state;
    sums_reset((uint8_t)(0x80 | st));
    bool watch = cc_link_watch_wanted();
    if (watch) cc_link_watch_resume(false);   /* one plain poll: no watch loop started on the Mac for a check-in */
    bool poll_ok;
    ts_checkin_t ts = bring_up(s_arg_rebind, &poll_ok, t0);
    if (poll_ok) wait_for(judged, JUDGED_WAIT_MS);
    ck_path_t path = !poll_ok ? P_FAILED : ts == TS_CHECKIN_REBIND ? P_REBIND : P_HANDSHAKE;
    bool needs = poll_ok && power_needs_you();
    record(path, (uint32_t)(now_ms() - t0), needs);
    if (want_parked(power_state())) {
        park_now();
    } else {
        /* a needs-you wake (Dim) or input meanwhile: stay up */
        ESP_LOGI(TAG, "check-in: staying up (%s)", power_state_name(power_state()));
    }
    if (watch) cc_link_watch_resume(true);   /* after the park: the link can't start a loop just to kill it (M2) */
    tailscale_set_fast_resume(false);   /* a later real Wi-Fi drop still rebinds */
}

/* The input wake from Radio off or Deep idle: steps 1-5, then stays up. */
static void job_unpark(void)
{
    int64_t t0 = now_ms();
    bool poll_ok;
    ts_checkin_t ts = bring_up(false, &poll_ok, t0);
    tailscale_set_fast_resume(false);
    ESP_LOGI(TAG, "wake reconnect: %s, poll %s, %lld ms", tailscale_checkin_name(ts), poll_ok ? "ok" : "failed",
             (long long)(now_ms() - t0));
    if (ts == TS_CHECKIN_FAILED) s_woke_parked = false;   /* stop saying "Reconnecting": the status says why */
}

static void job_park(void)
{
    park_now();
}

/* Nothing may be left pinned awake with no ladder (I1): the normal ladder takes over, its 10% trigger
   included. The idle clock already counts from before the sleep (checkin_init), so on battery it hibernates
   again by the hours rule; on USB it stays in Off. */
static void give_up(const char *why)
{
    ESP_LOGE(TAG, "hibernate check-in: %s: back to the normal ladder", why);
    s_hib_settled = true;
    s_hold = false;
    s_quiet = false;
    power_unpin();
}

/* Down for the next Hibernate check-in. The light is the power task's own choice (the Deep-idle rule under
   checkin_hib_quiet: amber at 30% only for needs-you with "Keyboard light shows needs-you" on, else off; I2),
   or off; held under the LED lock so the power task can't change it, then deep sleep with the check-in timer.
   A battery at or under 10% powers off instead, light off and no check-in armed (C1). Returns only if the
   hibernate failed, and then hands the device back to the ladder. */
static void down_with_light(bool amber)
{
    bool low = power_battery_low();
    pr_led_t l = PR_LED_OFF;
    if (amber && !low) power_kbd_light_get(&l);
    uint32_t s = low ? 0 : hibernate_checkin_s();
    keyboard_led_lock();
    keyboard_write_led(l.mode, l.bright, l.r, l.g, l.b);
    ESP_LOGI(TAG, "hibernate again: light %u %u %02x%02x%02x, %s", l.mode, l.bright, l.r, l.g, l.b,
             low ? "battery low: power-off, no check-in" : s ? "next check-in armed" : "check-in off");
    hibernate_now(s, s ? POWER_BOOT_HIB_CHECKIN : POWER_BOOT_HIB_WAKE);
    keyboard_led_unlock();
    give_up("the hibernate failed");
}

static bool hib_abandoned(void)
{
    static bool logged;
    if (s_quiet) return false;
    if (!logged) { logged = true; ESP_LOGI(TAG, "hibernate check-in abandoned: woken by input"); }
    return true;
}

/* PT16: the check-in boot. Screen dark from the start, 180 s in all. */
static void job_hib(void)
{
    int64_t t0 = 0;   /* the budget counts from boot */
    int64_t end = t0 + HIB_BUDGET_MS;
    sums_reset(0x85);
    uint8_t dbg = hibernate_boot_debug();
    /* C1: a nearly flat pack gets no Wi-Fi, no tunnel, no poll: off at once (the first INA226 reading first) */
    for (int i = 0; i < 25 && !sysinfo_power().valid; i++) vTaskDelay(pdMS_TO_TICKS(200));
    if (power_battery_low()) {
        record(P_HIB_LOW, (uint32_t)now_ms(), false);
        down_with_light(false);
        return;
    }
    bool poll_ok = false, needs = false;
    ts_checkin_t ts = TS_CHECKIN_FAILED;
    char mac[128];
    int64_t left = end - now_ms();
    if (wait_for(wifi_up, left < HIB_WIFI_WAIT_MS ? (int)left : HIB_WIFI_WAIT_MS) && !hib_abandoned() &&
        cc_link_mac_address(mac, sizeof(mac))) {
        ts = tailscale_checkin(mac, false, NULL);
        left = end - now_ms();
        if (ts != TS_CHECKIN_FAILED && !hib_abandoned() && left > 0)
            ts = link_and_poll(mac, ts, left < HIB_LINK_WAIT_MS ? (int)left : HIB_LINK_WAIT_MS, &poll_ok);
    }
    if (hib_abandoned()) { s_hib_settled = true; return; }
    if (poll_ok) {
        wait_for(judged, JUDGED_WAIT_MS);
        needs = power_needs_you();
    }
    bool late = now_ms() > end;
    if (late) poll_ok = false;   /* out of time: hibernate anyway, recorded as failed */
    ck_path_t path = !poll_ok ? P_HIB_FAILED : ts == TS_CHECKIN_REBIND ? P_HIB_REBIND : P_HIB_HANDSHAKE;
    if (power_battery_low()) path = P_HIB_LOW;
    record(path, (uint32_t)now_ms(), poll_ok && needs);
    bool kbd = hibernate_kbd_rail_survives();
    if (dbg & HIB_DBG_FORCE_KEEP) kbd = true;
    if (dbg & HIB_DBG_FORCE_NOKEEP) kbd = false;
    pr_hc_t next = pr_hib_checkin_decide(power_battery_low(), poll_ok, needs, kbd);
    static const char *const names[] = { "rehibernate", "rehibernate, light kept", "stay in deep idle",
                                         "power off (battery low)" };
    ESP_LOGI(TAG, "hibernate check-in: %s%s", names[next], late ? " (out of time)" : "");
    if (hib_abandoned()) return;
    if (next == PR_HC_STAY_DEEP_IDLE) {
        s_hib_settled = true;
        s_hold = true;
        s_hold_gen = cc_poll_generation();
        s_hold_gen_ms = now_ms();
        power_go(PS_DEEP_IDLE);   /* pinned: the ladder keeps only its low-battery trigger (checkin_hib_hold) */
        return;
    }
    if (dbg & HIB_DBG_STAY_AWAKE) {
        /* debug (`power checkin hibernate`): the console comes back; the light shows what it would hold */
        ESP_LOGW(TAG, "debug: would %s; staying awake", names[next]);
        s_hib_settled = true;
        power_unpin();   /* the ladder runs again (I1); the Deep-idle light rule stays until a wake */
        return;
    }
    down_with_light(next == PR_HC_REHIBERNATE_KEEP_RAIL);   /* POWEROFF: down_with_light sees the low battery */
}

static void job_down(void)
{
    s_hold = false;
    down_with_light(false);
}

static void job_task(void *arg)
{
    (void)arg;
    job_t j = s_job;
    ESP_LOGI(TAG, "job %s: start", k_job[j]);
    switch (j) {
    case JOB_PARK: job_park(); break;
    case JOB_UNPARK: job_unpark(); break;
    case JOB_CHECKIN: job_checkin(); break;
    case JOB_HIB: job_hib(); break;
    case JOB_DOWN: job_down(); break;
    default: break;
    }
    ESP_LOGI(TAG, "job %s: done (stack free %u B of %d)", k_job[j], (unsigned)uxTaskGetStackHighWaterMark(NULL), JOB_STACK);
    s_tag = 0;
    s_job = JOB_NONE;
    s_running = false;
    power_poll_kick();
    vTaskDelete(NULL);
}

static bool start(job_t j, power_state_t st, int64_t now)
{
    s_job = j;
    s_arg_state = st;
    s_running = true;
    s_job_started_ms = now;
    if (xTaskCreate(job_task, "checkin", JOB_STACK, NULL, 3, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the %s task", k_job[j]);
        s_running = false;
        s_job = JOB_NONE;
        return false;
    }
    power_poll_kick();   /* idle_poll samples at 1 Hz while a job runs */
    return true;
}

/* ---- the power task's part ---------------------------------------------------- */

static void schedule(power_state_t st, int64_t now)
{
    s_period = pr_checkin_period_min(st, power_cfg(PCFG_CHECKIN), s_deep_k);
    s_next_ms = s_period > 0 ? now + (int64_t)s_period * 60000 : 0;
}

/* Stuck past the budget (every wait in the job is bounded, so this is a backstop): down anyway. */
static void hib_watchdog_task(void *arg)
{
    (void)arg;
    ESP_LOGE(TAG, "hibernate check-in: over %d s, hibernating anyway", HIB_WATCHDOG_MS / 1000);
    if (!s_tag) sums_reset(0x85);
    record(P_HIB_FAILED, (uint32_t)now_ms(), false);   /* M3: the stuck case shows in `power checkins` */
    down_with_light(false);
    vTaskDelete(NULL);
}

void checkin_step(power_state_t st, int64_t now)
{
    if (st != s_prev) {
        if (st < PS_OFF) {
            /* a wake: the radio comes back at once (below), Deep idle's backoff starts again */
            if (s_prev >= PS_OFF) s_woke_parked = s_parked || s_running;
            s_deep_k = 0;
            s_next_ms = 0;
            s_force = false;   /* a forced check-in that never ran doesn't wait for the next park */
            if (s_quiet || s_hold) ESP_LOGI(TAG, "hibernate check-in: woken, back to normal");
            s_quiet = false;
            s_hold = false;
            s_hold_down = false;
        } else if (st == PS_DEEP_IDLE) {
            s_deep_k = 0;
            if (s_parked) schedule(st, now);
        } else if (st == PS_RADIO_OFF) {
            if (s_parked) schedule(st, now);
        }
        s_prev = st;
    }
    /* I1: a check-in boot that hasn't gone down or settled by HIB_WATCHDOG_MS goes down now, whether or
       not its job still runs (or ever started); a failed create is retried, then the ladder takes over */
    if (s_quiet && !s_hib_settled && !s_watchdog_fired && now > HIB_WATCHDOG_MS) {
        if (xTaskCreate(hib_watchdog_task, "ck_wd", JOB_STACK, NULL, 4, NULL) == pdPASS) s_watchdog_fired = true;
        else if (++s_watchdog_tries >= JOB_RETRIES) { s_watchdog_fired = true; give_up("no memory for the watchdog"); }
    }
    if (s_running) return;
    if (s_hib_pending) {
        if (start(JOB_HIB, st, now)) s_hib_pending = false;
        else if (++s_hib_tries >= JOB_RETRIES) { s_hib_pending = false; give_up("no memory for the check-in"); }
        return;
    }
    if (s_hold) {
        uint32_t gen = cc_poll_generation();
        bool fresh = gen != s_hold_gen;
        if (fresh) { s_hold_gen = gen; s_hold_gen_ms = now; }
        bool stale = now - s_hold_gen_ms > HOLD_STALE_MS;   /* the Mac unreachable for an hour: nothing to show */
        if (!s_hold_down && ((fresh && !power_needs_you()) || stale)) {
            ESP_LOGI(TAG, "hibernate check-in hold: %s", stale ? "no result for an hour" : "nobody needs you now");
            s_hold_down = true;
        }
        if (s_hold_down) {
            if (start(JOB_DOWN, st, now)) s_hold_down = false;
            else if (++s_down_tries >= JOB_RETRIES) { s_hold_down = false; give_up("no memory to hibernate"); }
            return;
        }
    }
    bool want = want_parked(st);
    if (want && !s_parked) {
        if (start(JOB_PARK, st, now)) {
            s_woke_parked = false;
            s_parked = true;   /* now, so the next step doesn't start it twice */
            schedule(st, now);
        }
        return;
    }
    if (!want && s_parked) {
        if (start(JOB_UNPARK, st, now)) s_parked = false;
        return;
    }
    if (want && s_parked && (s_force || (s_next_ms && now >= s_next_ms))) {
        bool rebind = s_force && s_force_rebind;
        s_force = false;
        s_arg_rebind = rebind;
        if (start(JOB_CHECKIN, st, now)) {
            if (st == PS_DEEP_IDLE) s_deep_k++;
            schedule(st, now);   /* the next one counts from this start: the period, not period + check-in */
        }
    }
}

void checkin_force(bool rebind)
{
    s_force_rebind = rebind;
    s_force = true;
    power_poll_kick();
    power_kbd_light_refresh();   /* notifies the power task */
}

void checkin_hibernate_test(int force_kbd)
{
    uint8_t dbg = HIB_DBG_STAY_AWAKE | (force_kbd == 1 ? HIB_DBG_FORCE_KEEP : force_kbd == 2 ? HIB_DBG_FORCE_NOKEEP : 0);
    hibernate_set_debug_next(dbg);
    printf("checkin: hibernating with a %d s wake (debug: a re-hibernate result stays awake%s)\n", HIB_TEST_WAKE_S,
           force_kbd == 1 ? ", keyboard rail forced kept" : force_kbd == 2 ? ", keyboard rail forced not kept" : "");
    fflush(stdout);
    vTaskDelay(pdMS_TO_TICKS(100));
    hibernate_now(HIB_TEST_WAKE_S, POWER_BOOT_HIB_CHECKIN);
    printf("checkin: the hibernate failed\n");
}

void checkin_init(void)
{
    log_load();
    s_inited = true;
    if (hibernate_boot_kind() == POWER_BOOT_HIB_CHECKIN) {
        s_quiet = true;        /* the Deep-idle light rule and no screen wake from the very first result */
        s_hib_pending = true;
        power_go(PS_OFF);      /* dark from the start, pinned */
        /* the device has been idle since before the sleep: if a fallback unpins it, the ladder sees that
           (on battery it hibernates again by the hours rule, on USB it stays in Off), never a fresh input */
        power_sim_idle(49 * 3600);
        ESP_LOGI(TAG, "hibernate check-in boot (debug 0x%02x)", hibernate_boot_debug());
    }
}
