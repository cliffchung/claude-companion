#include "screenshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include "lvgl.h"
#include "sys/power.h"
#include "esp_timer.h"

static const char *TAG = "shot";
/* Encode the active screen with the hardware JPEG engine and print it as a base64 frame. */
bool screenshot_dump_usb(void)
{
    bool ok = false;
    lv_draw_buf_t *snap = NULL;
    uint8_t *in = NULL, *out = NULL;
    jpeg_encoder_handle_t enc = NULL;
    uint32_t w = 0, h = 0, jpg_len = 0;
    size_t in_cap = 0, out_cap = 0;

    if (!lvgl_port_lock(0)) return false;
    int64_t locked_us = esp_timer_get_time();
    snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (snap) {
        w = snap->header.w; h = snap->header.h;
        jpeg_encode_memory_alloc_cfg_t ic = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        in = jpeg_alloc_encoder_mem((size_t)w * h * 3, &ic, &in_cap);
        if (in) {
            for (uint32_t y = 0; y < h; y++)
                memcpy(in + (size_t)y * w * 3, (const uint8_t *)snap->data + (size_t)y * snap->header.stride, (size_t)w * 3);
            /* lv_layer_top (toasts, the mini player, the sleep notice) is drawn over the screen */
            /* the transparent wake shield alone adds nothing: skip the second full-screen render */
            uint32_t top_n = lv_obj_get_child_count(lv_layer_top());
            lv_obj_t *shield = power_shield_obj();
            bool only_shield = top_n == 1 && shield && lv_obj_get_child(lv_layer_top(), 0) == shield;
            if (top_n > 0 && !only_shield) {
                lv_draw_buf_t *top = lv_snapshot_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888);
                if (top && top->header.w == w && top->header.h == h) {
                    for (uint32_t y = 0; y < h; y++) {
                        const uint8_t *t = (const uint8_t *)top->data + (size_t)y * top->header.stride;
                        uint8_t *d = in + (size_t)y * w * 3;
                        for (uint32_t x = 0; x < w; x++, t += 4, d += 3) {
                            uint32_t a = t[3];
                            if (!a) continue;
                            for (int c = 0; c < 3; c++) d[c] = (uint8_t)((t[c] * a + d[c] * (255 - a)) / 255);
                        }
                    }
                }
                if (top) lv_draw_buf_destroy(top);
            }
        }
    }
    lvgl_port_unlock();
    ESP_LOGI(TAG, "screenshot held the LVGL lock %lld ms", (long long)(esp_timer_get_time() - locked_us) / 1000);
    if (snap) { lv_draw_buf_destroy(snap); }
    if (!snap || !in) { ESP_LOGE(TAG, "snapshot/alloc failed"); goto done; }

    jpeg_encode_memory_alloc_cfg_t oc = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    out = jpeg_alloc_encoder_mem((size_t)w * h * 3 / 4 + 4096, &oc, &out_cap);
    if (!out) { ESP_LOGE(TAG, "out alloc failed"); goto done; }
    jpeg_encode_engine_cfg_t ec = { .timeout_ms = 5000 };
    if (jpeg_new_encoder_engine(&ec, &enc) != ESP_OK) { ESP_LOGE(TAG, "encoder init failed"); goto done; }
    jpeg_encode_cfg_t cfg = {
        .height = h, .width = w,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB888,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV420,
        .image_quality = 80,
    };
    if (jpeg_encoder_process(enc, &cfg, in, (uint32_t)in_cap, out, (uint32_t)out_cap, &jpg_len) != ESP_OK) {
        ESP_LOGE(TAG, "encode failed");
        goto done;
    }

    /* Base64 the whole image (57 input bytes -> 76 chars), then print with logging silenced. */
    size_t b64_cap = ((jpg_len + 56) / 57) * 77 + 8, b64_len = 0;
    char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM);
    if (!b64) { ESP_LOGE(TAG, "b64 alloc failed"); goto done; }
    size_t pos = 0;
    for (uint32_t off = 0; off < jpg_len; off += 57) {
        size_t n = jpg_len - off < 57 ? jpg_len - off : 57, olen = 0;
        mbedtls_base64_encode((unsigned char *)b64 + pos, b64_cap - pos, &olen, out + off, n);
        pos += olen;
        b64[pos++] = '\n';
    }
    b64_len = pos;
    esp_log_level_t prev = esp_log_level_get("*");
    esp_log_level_set("*", ESP_LOG_NONE);
    printf("\n@@SHOT jpeg %u %u %u\n", (unsigned)jpg_len, (unsigned)w, (unsigned)h);
    fwrite(b64, 1, b64_len, stdout);
    printf("@@END\n");
    fflush(stdout);
    esp_log_level_set("*", prev);
    free(b64);
    ok = true;
done:
    if (enc) jpeg_del_encoder_engine(enc);
    free(in);
    free(out);
    return ok;
}
