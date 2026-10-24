/* USB host for the HID keyboard. Plan 4 Task 1 removed the flash drive (MSC) and disk mode. */
#include "usb_mgr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/m5stack_tab5.h"
#include "usb/usb_host.h"
#include "usb/hid_host.h"
#include "usb/hid.h"
#include "input/keyboard.h"

static const char *TAG = "usb";
static bool s_host_started = false;
static TaskHandle_t s_lib_task = NULL;

/* ---------------- HID ---------------- */
static void hid_iface_cb(hid_host_device_handle_t dev, const hid_host_interface_event_t ev, void *arg)
{
    (void)arg;
    if (ev == HID_HOST_INTERFACE_EVENT_INPUT_REPORT) {
        uint8_t rep[64]; size_t len = 0;
        if (hid_host_device_get_raw_input_report_data(dev, rep, sizeof(rep), &len) == ESP_OK) {
            hid_host_dev_params_t p;
            if (hid_host_device_get_params(dev, &p) == ESP_OK && p.proto == HID_PROTOCOL_KEYBOARD) keyboard_usb_hid_report(rep, len);
        }
    } else if (ev == HID_HOST_INTERFACE_EVENT_DISCONNECTED) {
        hid_host_device_close(dev);
        ESP_LOGI(TAG, "HID device disconnected");
    }
}
static void hid_driver_cb(hid_host_device_handle_t dev, const hid_host_driver_event_t ev, void *arg)
{
    (void)arg;
    if (ev != HID_HOST_DRIVER_EVENT_CONNECTED) return;
    hid_host_dev_params_t p;
    if (hid_host_device_get_params(dev, &p) != ESP_OK) return;
    const hid_host_device_config_t cfg = { .callback = hid_iface_cb, .callback_arg = NULL };
    if (hid_host_device_open(dev, &cfg) != ESP_OK) return;
    if (p.sub_class == HID_SUBCLASS_BOOT_INTERFACE && p.proto == HID_PROTOCOL_KEYBOARD) {
        hid_class_request_set_protocol(dev, HID_REPORT_PROTOCOL_BOOT);
        hid_class_request_set_idle(dev, 0, 0);
        ESP_LOGI(TAG, "USB keyboard connected");
    }
    hid_host_device_start(dev);
}

/* The host library's event loop is ours rather than the BSP's: bsp_usb_host_stop()
 * uninstalls the library from under its own task, and bsp_usb.c's task wraps
 * usb_host_device_free_all() in ESP_ERROR_CHECK() -- that call returns
 * ESP_ERR_NOT_FINISHED whenever a device still has to be freed, which is the
 * normal answer rather than a failure, so the BSP aborts and the board reboots. */
static void usb_lib_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t flags = 0;
        esp_err_t e = usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (e == ESP_ERR_INVALID_STATE) break;
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            e = usb_host_device_free_all();
            if (e != ESP_OK && e != ESP_ERR_NOT_FINISHED) ESP_LOGW(TAG, "free all: %s", esp_err_to_name(e));
        }
    }
    s_lib_task = NULL;
    vTaskDelete(NULL);
}

static bool host_start(void)
{
    if (s_host_started) return true;
    if (bsp_feature_enable(BSP_FEATURE_USB, true) != ESP_OK) ESP_LOGW(TAG, "usb power enable failed");
    const usb_host_config_t hostcfg = { .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LOWMED };
    esp_err_t e = usb_host_install(&hostcfg);
    if (e != ESP_OK) { ESP_LOGW(TAG, "usb_host_install: %s", esp_err_to_name(e)); return false; }
    if (xTaskCreate(usb_lib_task, "usb_lib", 4096, NULL, 10, &s_lib_task) != pdPASS) {
        ESP_LOGE(TAG, "usb lib task failed");
        usb_host_uninstall();
        return false;
    }
    const hid_host_driver_config_t hc = { .create_background_task = true, .task_priority = 5, .stack_size = 4096, .core_id = 1, .callback = hid_driver_cb };
    if (hid_host_install(&hc) != ESP_OK) ESP_LOGW(TAG, "hid host install failed");
    s_host_started = true;
    return true;
}

void usb_mgr_init(void)
{
    host_start();
}
