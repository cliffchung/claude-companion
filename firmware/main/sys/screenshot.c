#include "screenshot.h"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "driver/jpeg_encode.h"
#include "mbedtls/base64.h"
#include "lvgl.h"
#include "esp_lcd_touch.h"
#include "bsp/m5stack_tab5.h"
#include "storage/sd.h"
#include "ui/ui.h"
#include "sys/display_ctl.h"

static const char *TAG = "shot";
static bool s_armed = true;

static bool write_bmp(const char *path, lv_draw_buf_t *buf)
{
    uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    lv_color_format_t cf = buf->header.cf;
    int bpp = lv_color_format_get_bpp(cf) / 8;
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    uint32_t row_bytes = (w * 3 + 3) & ~3u;
    uint32_t data = row_bytes * h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t fsz = 54 + data;
    memcpy(hdr + 2, &fsz, 4); uint32_t off = 54; memcpy(hdr + 10, &off, 4);
    uint32_t ih = 40; memcpy(hdr + 14, &ih, 4); memcpy(hdr + 18, &w, 4); memcpy(hdr + 22, &h, 4);
    uint16_t planes = 1, bits = 24; memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bits, 2);
    memcpy(hdr + 34, &data, 4);
    fwrite(hdr, 1, 54, f);
    uint8_t *row = malloc(row_bytes);
    memset(row, 0, row_bytes);
    for (int y = (int)h - 1; y >= 0; y--) {
        const uint8_t *src = (const uint8_t *)buf->data + y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r, g, b;
            if (bpp == 2) { uint16_t p = ((const uint16_t *)src)[x]; r = ((p >> 11) & 31) << 3; g = ((p >> 5) & 63) << 2; b = (p & 31) << 3; }
            else if (bpp == 3) { b = src[x * 3]; g = src[x * 3 + 1]; r = src[x * 3 + 2]; }
            else { b = src[x * 4]; g = src[x * 4 + 1]; r = src[x * 4 + 2]; }
            row[x * 3] = b; row[x * 3 + 1] = g; row[x * 3 + 2] = r;
        }
        fwrite(row, 1, row_bytes, f);
    }
    free(row);
    fclose(f);
    return true;
}

bool screenshot_take(char *out_path, int cap)
{
    if (!sd_is_mounted()) return false;
    fs_mkdir_p(SD_MOUNT "/ScreenShots");
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char path[96];
    strftime(path, sizeof(path), SD_MOUNT "/ScreenShots/%Y%m%d_%H%M%S.bmp", &tm);
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    if (!buf) { ESP_LOGE(TAG, "snapshot failed"); return false; }
    bool ok = write_bmp(path, buf);
    lv_draw_buf_destroy(buf);
    if (ok && out_path) strlcpy(out_path, path, cap);
    ESP_LOGI(TAG, "screenshot %s: %s", path, ok ? "ok" : "failed");
    return ok;
}

static void poll_cb(lv_timer_t *t)
{
    (void)t;
    /* asked each time: a re-created touch driver (Task 6) gets a new handle */
    esp_lcd_touch_handle_t tp = display_ctl_touch();
    if (!tp) return;
    uint16_t x[5], y[5], s[5]; uint8_t cnt = 0;
    /* data is refreshed by the LVGL port's own read callback */
    bool got = esp_lcd_touch_get_coordinates(tp, x, y, s, &cnt, 5);
    if (got && cnt >= 3) {
        if (s_armed) {
            s_armed = false;
            char p[96];
            if (screenshot_take(p, sizeof(p))) ui_toast(tr(STR_SCREENSHOT_SAVED)); else ui_toast(tr(STR_SCREENSHOT_FAILED));
        }
    } else if (!got || cnt == 0) {
        s_armed = true;
    }
}

void screenshot_init(void)
{
    lv_timer_create(poll_cb, 60, NULL);
}

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
    snap = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (snap) {
        w = snap->header.w; h = snap->header.h;
        jpeg_encode_memory_alloc_cfg_t ic = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
        in = jpeg_alloc_encoder_mem((size_t)w * h * 3, &ic, &in_cap);
        if (in) {
            for (uint32_t y = 0; y < h; y++)
                memcpy(in + (size_t)y * w * 3, (const uint8_t *)snap->data + (size_t)y * snap->header.stride, (size_t)w * 3);
            /* lv_layer_top (toasts, the mini player, the sleep notice) is drawn over the screen */
            if (lv_obj_get_child_count(lv_layer_top()) > 0) {
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
