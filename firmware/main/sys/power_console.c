#include "power_console.h"
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
           "power bench start [fast]|status|report|abort | power light [test bind|user|reprobe] | "
           "power go active|dim|off|deep-idle|hibernate | power set dim|off|sleep-usb|needs-wake|deep-idle|hibernate <n> | "
           "power set defaults | power settings | power sim battery <pct|off> | power sim idle <s> | power sim touch | "
           "power touch | power wakes\n");
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
        { "active", PS_ACTIVE }, { "dim", PS_DIM }, { "off", PS_OFF }, { "deep-idle", PS_DEEP_IDLE },
        { "hibernate", PS_HIBERNATE },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if (strcmp(name, k[i].n)) continue;
        if (k[i].s == PS_HIBERNATE && !hibernate_supported())
            printf("hibernate: not built (the notice shows for 3 s, then the device goes back to off)\n");
        power_go(k[i].s);
        printf("power: go %s\n", k[i].n);
        return 0;
    }
    return usage();
}

static int cmd_power(int argc, char **argv)
{
    if (argc == 1) {
        power_info_t p = sysinfo_power();
        char pct[8] = "-", holds[48];
        if (p.percent >= 0) snprintf(pct, sizeof(pct), "%d%%", p.percent);
        power_holds_text(holds, sizeof(holds));
        const char *st = power_state_name(power_state());
        long idle = (long)(power_idle_ms() / 1000);
        if (!p.valid) {
            printf("power: - V - mA - W %s charging=- src=%s chg_pin=%d state=%s idle=%lds holds=%s\n", pct,
                   src_name(p.src), sysinfo_charge_pin(), st, idle, holds);
            return 0;
        }
        printf("power: %.2f V %d mA %.2f W %s charging=%s src=%s chg_pin=%d state=%s idle=%lds holds=%s\n",
               p.bus_v, (int)lroundf(p.current_a * 1000.0f), p.bus_v * fabsf(p.current_a), pct,
               p.src == PWR_USB_CHARGING ? "yes" : "no", src_name(p.src), sysinfo_charge_pin(), st, idle, holds);
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
        if (!parse_int(argv[3], &n) || !power_setting_set(argv[2], n)) return usage();
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
        if (strcmp(argv[2], "touch") == 0 && argc == 3) { power_sim_touch(); return 0; }
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

void power_console_init(void)
{
    const esp_console_cmd_t clk = {
        .command = "clock",
        .help = "clock | tz <rule> | at <epoch>",
        .func = cmd_clock,
    };
    debug_console_register(&clk);
    const esp_console_cmd_t cmd = {
        .command = "power",
        .help = "power | log on|off|status|dump|clear | resets [clear] | bench start [fast]|status|report|abort | "
                "light [test bind|user|reprobe] | go <state> | set <name> <n> | set defaults | settings | "
                "sim battery <pct|off> | sim idle <s> | sim touch | touch | wakes",
        .func = cmd_power,
    };
    debug_console_register(&cmd);
}
