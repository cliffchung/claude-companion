#pragma once
/* Host-test stand-in for ESP-IDF's esp_log.h: the log calls print nothing (their arguments are still used). */
static inline void stub_esp_log(const char *tag, const char *fmt, ...) { (void)tag; (void)fmt; }
#define ESP_LOGE(tag, ...) stub_esp_log(tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) stub_esp_log(tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) stub_esp_log(tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) stub_esp_log(tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) stub_esp_log(tag, __VA_ARGS__)
