#include "debug_console.h"
#include "screenshot.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "console";
static bool s_started = false;

static int cmd_screenshot(int argc, char **argv)
{
    (void)argc; (void)argv;
    return screenshot_dump_usb() ? 0 : 1;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc; (void)argv;
    esp_restart();
    return 0;
}

esp_err_t debug_console_register(const esp_console_cmd_t *cmd)
{
    return esp_console_cmd_register(cmd);
}

void debug_console_start(void)
{
    if (s_started) return;
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "tab5> ";
    rc.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t dc = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    if (esp_console_new_repl_usb_serial_jtag(&dc, &rc, &repl) != ESP_OK) {
        ESP_LOGE(TAG, "repl init failed");
        return;
    }
    esp_console_register_help_command();
    const esp_console_cmd_t shot = {
        .command = "screenshot",
        .help = "Dump the screen as a base64 JPEG frame (@@SHOT ... @@END)",
        .hint = NULL,
        .func = cmd_screenshot,
    };
    const esp_console_cmd_t reboot = {
        .command = "reboot",
        .help = "Restart the device",
        .hint = NULL,
        .func = cmd_reboot,
    };
    debug_console_register(&shot);
    debug_console_register(&reboot);
    if (esp_console_start_repl(repl) != ESP_OK) {
        ESP_LOGE(TAG, "repl start failed");
        return;
    }
    s_started = true;
}
