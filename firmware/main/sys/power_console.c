#include "power_console.h"
#include "sys/url_mask.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "esp_console.h"
#include "sys/debug_console.h"
#include "sys/sysinfo.h"
#include "sys/power_log.h"
#include "sys/power_bench.h"
#include "sys/vitals.h"
#include "sys/power.h"
#include "sys/hibernate.h"
#include "sys/checkin.h"
#include "sys/boot_phase.h"
#include "net/wifi_mgr.h"
#include "input/keyboard.h"
#include "settings/settings.h"

static const char *src_name(power_src_t s)
{
    switch (s) {
    case PWR_USB: return "usb";
    case PWR_USB_CHARGING: return "usb-charging";
    case PWR_USB_STANDBY: return "usb-standby";
    case PWR_BATTERY: return "battery";
    default: return "unknown";
    }
}

static int usage(void)
{
    printf("usage: power | power log on|off|status|dump|clear | power resets [clear] | "
           "power bench start [fast]|status|report|abort | power light [test bind|user|reprobe | raw <mode> <bright> <r> <g> <b>] | "
           "power go active|dim|off|radio-off|deep-idle|hibernate | "
           "power set dim|off|sleep-usb|needs-wake|deep-idle|hibernate|radio-off|radio-after|checkin|hib-checkin <n> | "
           "power checkin [rebind] | power checkin hibernate [keep|nokeep] [usb] [chain <n>] [exp <mask>] | power checkins | power phases [clear] | "
           "power set defaults | power settings | power sim battery <pct|off> | power sim idle <s> | power sim touch [<x> <y>] | "
           "power touch | power wakes | power hib [test timer|rtc|off|light [s] | test-wake <s> | backstop [<s> <n>] | probe <s> <n>] | power boot | "
           "power rails | power rail speaker|camera|usb|ext5v on|off | power imu [suspend]\n");
    return 1;
}

static bool parse_int(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno || !*s || *end || v < -1000000 || v > 1000000) return false;
    *out = (int)v;
    return true;
}

static int cmd_go(const char *name)
{
    static const struct { const char *n; power_state_t s; } k[] = {
        { "active", PS_ACTIVE }, { "dim", PS_DIM }, { "off", PS_OFF }, { "radio-off", PS_RADIO_OFF }, { "deep-idle", PS_DEEP_IDLE },
        { "hibernate", PS_HIBERNATE },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(name, k[i].n)) continue;
        /* PT26: every hibernate is a deep sleep with a timer (a check-in, or the power probe, which boots on
           USB), so the console needs no test-wake first */
        power_go(k[i].s);
        printf("power: go %s\n", k[i].n);
        return 0;
    }
    return usage();
}

/* The radio and the check-ins (Task 8): in Radio off and Deep idle, next-checkin=<s> and period=<min>. */
static void radio_line(void)
{
    char rc[96];
    wifi_mgr_reconnect_text(rc, sizeof(rc));
    power_state_t st = power_state();
    if (st == PS_RADIO_OFF || st == PS_DEEP_IDLE)
        printf("radio: %s next-checkin=%d period=%d%s%s\n", rc, checkin_next_s(), checkin_period_min(),
               checkin_busy() ? " (a job is running)" : "", checkin_hib_hold() ? " hib-checkin-hold" : "");
    else
        printf("radio: %s%s\n", rc, checkin_busy() ? " (a job is running)" : "");
}

static int cmd_checkin(int argc, char **argv)
{
    if (argc == 2) { checkin_force(false); printf("checkin: one check-in now\n"); }
    else if (argc == 3 && strcmp(argv[2], "rebind") == 0) { checkin_force(true); printf("checkin: one check-in now, rebind forced\n"); }
    else if (argc >= 3 && argc <= 9 && strcmp(argv[2], "hibernate") == 0) {
        int force = 0;
        uint32_t chain = 0;
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "exp") == 0 && i + 1 < argc) {
                boot_phase_exp_set((uint32_t)strtoul(argv[++i], NULL, 0));   /* Task 3 A/B: BPX_* bits */
            } else if (strcmp(argv[i], "chain") == 0 && i + 1 < argc) {
                /* Task 3 timing: n more check-in boots go down again with the test timer */
                chain = (uint32_t)atoi(argv[++i]);
                if (chain > 8) return usage();
            } else if (strcmp(argv[i], "keep") == 0) force |= 1;
            else if (strcmp(argv[i], "nokeep") == 0) force |= 2;
            else if (strcmp(argv[i], "usb") == 0) force |= 0x10;   /* on USB, check in as on battery (timing) */
            else return usage();
        }
        if ((force & 3) == 3) return usage();
        boot_phase_chain_set(chain);
        checkin_hibernate_test(force);
        return 1;   /* only if the hibernate failed */
    } else return usage();
    power_state_t st = power_state();
    if (!(st == PS_RADIO_OFF || (st == PS_DEEP_IDLE && power_cfg(PCFG_RADIO_OFF))))
        printf("checkin: it runs once the radio is parked (radio-off, or deep-idle with radio-off 1); state=%s\n",
               power_state_name(st));
    return 0;
}

static int cmd_power(int argc, char **argv)
{
    int h = argc >= 2 ? hibernate_console(argc, argv) : -1;   /* hib, boot, rails, rail, imu */
    if (h >= 0) return h;
    if (argc == 1) {
        power_info_t p = sysinfo_power();
        char pct[8] = "-", holds[48];
        if (p.percent >= 0) snprintf(pct, sizeof(pct), "%d%%", p.percent);
        power_holds_text(holds, sizeof(holds));
        const char *st = power_state_name(power_state());
        long idle = (long)(power_idle_ms() / 1000);
        if (!p.valid) {
            printf("power: - V - mA - W %s charging=- src=%s chg_en=%d chg_pin=%d state=%s idle=%lds holds=%s\n", pct,
                   src_name(p.src), hibernate_chg_en(), sysinfo_charge_pin(), st, idle, holds);
            radio_line();
            return 0;
        }
        printf("power: %.2f V %d mA %.2f W %s charging=%s src=%s chg_en=%d chg_pin=%d state=%s idle=%lds holds=%s\n",
               p.bus_v, (int)lroundf(p.current_a * 1000.0f), p.bus_v * fabsf(p.current_a), pct,
               sysinfo_charging() ? "yes" : "no", src_name(p.src), hibernate_chg_en(), sysinfo_charge_pin(), st, idle,
               holds);
        radio_line();
        return 0;
    }
    if (strcmp(argv[1], "log") == 0 && argc == 3) {
        if (strcmp(argv[2], "on") == 0) { power_log_enable(true); printf("power log: on\n"); return 0; }
        if (strcmp(argv[2], "off") == 0) { power_log_enable(false); printf("power log: off\n"); return 0; }
        if (strcmp(argv[2], "status") == 0) { power_log_status(); return 0; }
        if (strcmp(argv[2], "dump") == 0) { power_log_dump(); return 0; }
        if (strcmp(argv[2], "clear") == 0) { power_log_clear(); printf("power log: cleared\n"); return 0; }
    } else if (strcmp(argv[1], "resets") == 0 && argc <= 3) {
        if (argc == 3) {
            if (strcmp(argv[2], "clear") != 0) return usage();
            power_log_resets_clear();
            printf("power resets: cleared\n");
        } else {
            power_log_resets();
        }
        return 0;
    } else if (strcmp(argv[1], "light") == 0) {
        if (argc == 1 + 1) {
            pr_led_t l;
            char why[128];
            power_kbd_light_explain(&l, why, sizeof(why));
            printf("light: %u %u %02x %02x %02x (%s)\n", l.mode, l.bright, l.r, l.g, l.b, why);
            uint8_t m, br, r, g, b;
            keyboard_get_last_led(&m, &br, &r, &g, &b);
            printf("kbd: %u %u %02x %02x %02x\n", m, br, r, g, b);
            if (keyboard_read_led(&m, &br, &r, &g, &b)) printf("a164: %u %u %02x %02x %02x\n", m, br, r, g, b);
            else printf("a164: no answer\n");
            return 0;
        }
        if (argc == 8 && strcmp(argv[2], "raw") == 0) {
            int v[5];
            for (int i = 0; i < 5; i++)
                if (!parse_int(argv[3 + i], &v[i]) || v[i] < 0 || v[i] > 255) return usage();
            bool ok = power_kbd_light_raw((uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], (uint8_t)v[3], (uint8_t)v[4]);
            uint8_t m, br, r, g, b;
            if (ok && keyboard_read_led(&m, &br, &r, &g, &b))
                printf("raw: a164 %u %u %02x %02x %02x (held until power light test user)\n", m, br, r, g, b);
            else printf("raw: no keyboard\n");
            return 0;
        }
        if (argc == 4 && strcmp(argv[2], "test") == 0) {
            if (strcmp(argv[3], "bind") == 0) power_kbd_light_test(POWER_LIGHT_TEST_BIND);
            else if (strcmp(argv[3], "user") == 0) power_kbd_light_test(POWER_LIGHT_TEST_USER);
            else if (strcmp(argv[3], "reprobe") == 0) power_kbd_light_test(POWER_LIGHT_TEST_REPROBE);
            else return usage();
            printf("ok\n");
            return 0;
        }
    } else if (strcmp(argv[1], "checkin") == 0) {
        return cmd_checkin(argc, argv);
    } else if (strcmp(argv[1], "phases") == 0 && argc <= 3) {
        if (argc == 3 && strcmp(argv[2], "clear") == 0) { boot_phase_clear(); printf("phases: cleared\n"); }
        else boot_phase_print();
        return 0;
    } else if (strcmp(argv[1], "checkins") == 0 && argc == 2) {
        checkin_print();
        return 0;
    } else if (strcmp(argv[1], "go") == 0 && argc == 3) {
        return cmd_go(argv[2]);
    } else if (strcmp(argv[1], "set") == 0 && argc == 3 && strcmp(argv[2], "defaults") == 0) {
        power_settings_defaults();
        char b[320];
        power_settings_text(b, sizeof(b));
        printf("%s\n", b);
        return 0;
    } else if (strcmp(argv[1], "set") == 0 && argc == 4) {
        int n;
        if (!parse_int(argv[3], &n)) return usage();
        /* the Task 8 names are test values, RAM only (P93); the others save as before */
        if (!power_setting_set_ram(argv[2], n) && !power_setting_set(argv[2], n)) return usage();
        char b[320];
        power_settings_text(b, sizeof(b));
        printf("%s\n", b);
        return 0;
    } else if (strcmp(argv[1], "settings") == 0 && argc == 2) {
        char b[320];
        power_settings_text(b, sizeof(b));
        printf("%s\n", b);
        return 0;
    } else if (strcmp(argv[1], "sim") == 0 && argc >= 3) {
        int n;
        if (strcmp(argv[2], "touch") == 0 && argc == 3) { power_sim_touch(-1, -1); return 0; }
        if (strcmp(argv[2], "touch") == 0 && argc == 5) {   /* at a point: the terminal's top tap zone (Task 10) */
            int x, y;
            if (!parse_int(argv[3], &x) || !parse_int(argv[4], &y) || x < 0 || y < 0) return usage();
            power_sim_touch(x, y);
            return 0;
        }
        if (strcmp(argv[2], "battery") == 0 && argc == 4) {
            if (strcmp(argv[3], "off") == 0) { power_sim_battery(-1); printf("sim battery: off\n"); return 0; }
            if (!parse_int(argv[3], &n) || n < 0 || n > 100) return usage();
            power_sim_battery(n);
            printf("sim battery: %d%% on battery\n", n);
            return 0;
        }
        if (strcmp(argv[2], "idle") == 0 && argc == 4) {
            if (!parse_int(argv[3], &n) || n < 0) return usage();
            power_sim_idle(n);
            printf("sim idle: +%ds\n", n);
            return 0;
        }
    } else if (strcmp(argv[1], "touch") == 0 && argc == 2) {
        char b[160];
        power_touch_text(b, sizeof(b));
        printf("%s\n", b);
        return 0;
    } else if (strcmp(argv[1], "wakes") == 0 && argc == 2) {
        power_wakes_print();
        return 0;
    } else if (strcmp(argv[1], "bench") == 0 && argc >= 3) {
        if (strcmp(argv[2], "start") == 0 && argc <= 4) {
            if (argc == 4 && strcmp(argv[3], "fast") != 0) return usage();
            power_bench_start(argc == 4);
            return 0;
        }
        if (argc == 3 && strcmp(argv[2], "status") == 0) { power_bench_status(); return 0; }
        if (argc == 3 && strcmp(argv[2], "report") == 0) { power_bench_report(); return 0; }
        if (argc == 3 && strcmp(argv[2], "abort") == 0) { power_bench_abort(); return 0; }
    }
    return usage();
}

static void clock_print(time_t t)
{
    struct tm tm;
    char b[40];
    if (!localtime_r(&t, &tm)) { printf("clock: epoch out of range\n"); return; }
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S %Z", &tm);
    printf("clock: %s tz=%s\n", b, g_settings.tz_rule);
}

static int cmd_clock(int argc, char **argv)
{
    if (argc == 1) { clock_print(time(NULL)); return 0; }
    if (argc == 3 && strcmp(argv[1], "tz") == 0) {
        if (!vitals_tz_valid(argv[2])) { printf("clock: invalid time zone rule\n"); return 1; }
        strcpy(g_settings.tz_rule, argv[2]);   /* valid rules are at most 47 characters */
        settings_save();
        sysinfo_apply_timezone();
        clock_print(time(NULL));
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "at") == 0) {
        char *end;
        errno = 0;
        long long e = strtoll(argv[2], &end, 10);
        if (errno == ERANGE) { printf("clock: epoch out of range\n"); return 1; }
        if (*argv[2] == 0 || *end) { printf("clock: not an epoch\n"); return 1; }
        clock_print((time_t)e);
        return 0;
    }
    printf("usage: clock | clock tz <rule> | clock at <epoch>\n");
    return 1;
}

/* `ota url [<url>|none]`: the OTA manifest URL (no Settings row since Plan 4). It may carry a token in its
   userinfo or query, which print masked (url_mask). No argument prints the current one. */
static void ota_url_print(const char *u)
{
    char b[200];
    url_mask(u, b, sizeof(b));   /* userinfo and a query may hold a token */
    printf("ota url: %s\n", b);
}

static int cmd_ota(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "url") == 0 && argc <= 3) {
        if (argc == 3) {
            const char *v = argv[2];
            if (strcmp(v, "none") == 0) v = "";
            else if ((strncmp(v, "https://", 8) && strncmp(v, "http://", 7)) || strlen(v) >= sizeof(g_settings.ota_url)) {
                printf("ota url: use http(s)://... (under %u characters) or none\n", (unsigned)sizeof(g_settings.ota_url));
                return 1;
            }
            strcpy(g_settings.ota_url, v);
            settings_save();
        }
        ota_url_print(g_settings.ota_url);
        return 0;
    }
    printf("usage: ota url [<url>|none]\n");
    return 1;
}

void power_console_init(void)
{
    const esp_console_cmd_t ota = {
        .command = "ota",
        .help = "ota url [<url>|none]: the update manifest URL (a query is masked when printed)",
        .func = cmd_ota,
    };
    debug_console_register(&ota);
    const esp_console_cmd_t clk = {
        .command = "clock",
        .help = "clock | tz <rule> | at <epoch>",
        .func = cmd_clock,
    };
    debug_console_register(&clk);
    const esp_console_cmd_t cmd = {
        .command = "power",
        .help = "power | log on|off|status|dump|clear | resets [clear] | bench start [fast]|status|report|abort | "
                "light [test bind|user|reprobe|raw m b r g b] | go <state> | set <name> <n> | set defaults | settings | "
                "checkin [rebind] | checkin hibernate [keep|nokeep] | checkins | "
                "sim battery <pct|off> | sim idle <s> | sim touch [x y] | touch | wakes | hib ... | boot | rails | rail <n> on|off | imu",
        .func = cmd_power,
    };
    debug_console_register(&cmd);
}
