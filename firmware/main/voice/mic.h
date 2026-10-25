/* The ES7210 mic capture (Plan 5 Task 5, spec 9.2). Nothing exists at boot: mic_open builds an RX-only I2S channel
   and an esp_codec_dev input device for one recording, mic_close tears both down (MCLK stops, DMA buffers freed).
   One user at a time (the voice task or the `voice rec` console verb); not for the LVGL task. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIC_GAIN_DB 30   /* Step 2 sets the measured value */
#define MIC_PICK    (-1) /* -1: average both slots; 0 or 1: that slot only (Step 2) */
typedef struct { int rate; int gain_db; } mic_cfg_t;           /* { VR_RATE, MIC_GAIN_DB } */
/* Creates an RX-only I2S channel on CONFIG_BSP_I2S_NUM (std Philips, 16-bit, stereo slots, BSP pins MCLK/BCLK/WS/DIN,
   dma_desc_num 4, dma_frame_num 400), an esp_codec_dev input device on it (audio_codec_new_i2s_data,
   audio_codec_new_i2c_ctrl at ES7210_CODEC_DEFAULT_ADDR on bsp_i2c_get_handle(), es7210_codec_new), opens it at
   cfg->rate with 2 channels and sets the gain. ESP_OK or the failing step's error, with everything undone. */
esp_err_t mic_open(const mic_cfg_t *cfg);
/* Blocking read of frames stereo frames (2 * frames int16) within timeout_ms; returns frames read, or -1. */
int mic_read(int16_t *buf, size_t frames, int timeout_ms);
/* esp_codec_dev_close + delete, then i2s_channel_disable + i2s_del_channel: MCLK stops. Safe when not open. */
void mic_close(void);
bool mic_is_open(void);

#ifdef __cplusplus
}
#endif
