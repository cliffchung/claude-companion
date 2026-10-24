#include "link_down.h"
#include "link_guard.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net/tailscale.h"
#include "ssh/ssh_client.h"

static const char *TAG = "link_down";

static int64_t clk_now(void) { return esp_timer_get_time() / 1000; }
static void clk_sleep(int ms) { vTaskDelay(pdMS_TO_TICKS(ms > 0 ? ms : 1)); }
static const lg_clock_t s_clk = { clk_now, clk_sleep };

/* Tailscale off (the switch, `ts off`, a node forget): close the sessions over tailnet
   addresses while the tunnel still carries their close, then stop it; the link reconnects on the LAN. */
static void ts_stop_hook(void (*stop)(void *ctx), void *ctx)
{
    lg_token_t t = lg_close(true, LG_CLOSE_MS);
    ESP_LOGI(TAG, "tailscale off: tailnet SSH %s (%d ms)", t.idle ? "closed" : "still busy", t.waited_ms);
    stop(ctx);
    lg_reopen(t);
}

void link_down_init(void)
{
    lg_set_clock(&s_clk);
    ssh_client_guard_init();
    tailscale_set_stop_hook(ts_stop_hook);
}

void link_down_restart(const char *why)
{
    ESP_LOGW(TAG, "restart (%s): closing SSH first", why ? why : "?");
    lg_token_t t = lg_close(false, LG_CLOSE_MS);
    ESP_LOGI(TAG, "SSH %s (%d ms); restarting", t.idle ? "closed" : "still busy", t.waited_ms);
    vTaskDelay(pdMS_TO_TICKS(150));   /* the FIN goes out over the radio before the reset */
    esp_restart();
}
