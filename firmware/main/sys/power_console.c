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
           "power bench start [fast]|status|report|abort | power light [test bind|user|reprobe]\n");
    return 1;
}

static int cmd_power(int argc, char **argv)
{
    if (argc == 1) {
        power_info_t p = sysinfo_power();
        /* state, idle and holds belong to the power state machine (Task 5) */
        char pct[8] = "-";
        if (p.percent >= 0) snprintf(pct, sizeof(pct), "%d%%", p.percent);
        if (!p.valid) {
            printf("power: - V - mA - W %s charging=- src=%s chg_pin=%d state=- idle=- holds=-\n", pct,
                   src_name(p.src), sysinfo_charge_pin());
            return 0;
        }
        printf("power: %.2f V %d mA %.2f W %s charging=%s src=%s chg_pin=%d state=- idle=- holds=-\n",
               p.bus_v, (int)lroundf(p.current_a * 1000.0f), p.bus_v * fabsf(p.current_a), pct,
               p.src == PWR_USB_CHARGING ? "yes" : "no", src_name(p.src), sysinfo_charge_pin());
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
        .help = "power | log on|off|status|dump|clear | resets [clear] | bench start [fast]|status|report|abort | light [test bind|user|reprobe]",
        .func = cmd_power,
    };
    debug_console_register(&cmd);
}
