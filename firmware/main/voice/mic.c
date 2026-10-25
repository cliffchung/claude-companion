/* The ES7210 mic capture (Plan 5 Task 5, spec 9.2): an RX-only I2S channel and an esp_codec_dev input device that
   exist only between mic_open and mic_close.

   Why not bsp_audio_init(): the BSP makes a duplex TX+RX pair on CONFIG_BSP_I2S_NUM, enables both, keeps the
   handles private and has no deinit, so MCLK would run for good after the first recording. Here the RX channel is
   the I2S master on its own (it drives MCLK G30, BCLK G27 and WS G29 itself; no TX channel is needed for the
   clocks), the ES7210 is an I2S slave clocked from that MCLK (256 x fs, the driver's default divider), and it needs
   no power pin or GPIO (the BSP's own bsp_audio_codec_microphone_init passes none). The ES8388 and the speaker amp
   are never touched.

   Reads go straight to i2s_channel_read so the caller's timeout holds (esp_codec_dev_read waits a fixed 1 s). */
#include "voice/mic.h"
#include <string.h>
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "bsp/m5stack_tab5.h"

static const char *TAG = "mic";

/* The BSP's own choice (es7210 driver default): ADC1 and ADC2 in the two I2S slots, no TDM. */
#define MIC_SELECT    (ES7210_SEL_MIC1 | ES7210_SEL_MIC2)
#define MIC_DMA_DESC  4
#define MIC_DMA_FRAME 400
/* The ES7210 puts out a pop 100-200 ms after it is enabled (device pass A: block 1 read about 20 dB over the room
   floor on every open, at every gain). mic_read drops the first MIC_SETTLE_MS of frames, so the pop never reaches
   the recording, the meter or the silence gate. */
#define MIC_SETTLE_MS 250

static i2s_chan_handle_t s_rx;
static const audio_codec_data_if_t *s_data;
static const audio_codec_ctrl_if_t *s_ctrl;
static const audio_codec_if_t *s_codec;
static esp_codec_dev_handle_t s_dev;
static volatile bool s_open;
static bool s_dev_opened;                /* esp_codec_dev_open succeeded: its close disables the channel */
static size_t s_skip;                     /* frames still to drop (MIC_SETTLE_MS after open) */

/* Undoes whatever exists, in reverse order of creation. */
static void teardown(void)
{
    bool disabled = false;
    if (s_dev) {
        esp_codec_dev_close(s_dev);       /* ES7210 off (disable_when_closed), the RX channel disabled */
        esp_codec_dev_delete(s_dev);
        s_dev = NULL;
        disabled = s_dev_opened;
        s_dev_opened = false;
    }
    if (s_codec) { audio_codec_delete_codec_if(s_codec); s_codec = NULL; }   /* uses s_ctrl: before it */
    if (s_ctrl) { audio_codec_delete_ctrl_if(s_ctrl); s_ctrl = NULL; }       /* removes the I2C device */
    if (s_data) { audio_codec_delete_data_if(s_data); s_data = NULL; }
    if (s_rx) {
        if (!disabled) (void)i2s_channel_disable(s_rx);  /* the codec close already did it (and a second call logs E) */
        i2s_del_channel(s_rx);            /* MCLK stops, the DMA buffers are freed */
        s_rx = NULL;
    }
}

esp_err_t mic_open(const mic_cfg_t *cfg)
{
    if (!cfg || cfg->rate <= 0) return ESP_ERR_INVALID_ARG;
    if (s_open || s_rx) return ESP_ERR_INVALID_STATE;
    esp_err_t e;
    const char *step;

    step = "i2s channel";
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = MIC_DMA_DESC;
    chan_cfg.dma_frame_num = MIC_DMA_FRAME;
    e = i2s_new_channel(&chan_cfg, NULL, &s_rx);
    if (e != ESP_OK) { s_rx = NULL; goto fail; }

    step = "i2s std mode";
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG((uint32_t)cfg->rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws = BSP_I2S_LCLK,
            .dout = I2S_GPIO_UNUSED,
            .din = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    e = i2s_channel_init_std_mode(s_rx, &std_cfg);
    if (e != ESP_OK) goto fail;

    step = "codec data";
    audio_codec_i2s_cfg_t i2s_cfg = { .port = CONFIG_BSP_I2S_NUM, .rx_handle = s_rx, .tx_handle = NULL };
    s_data = audio_codec_new_i2s_data(&i2s_cfg);
    if (!s_data) { e = ESP_ERR_NO_MEM; goto fail; }

    step = "codec i2c";
    audio_codec_i2c_cfg_t i2c_cfg = { .port = BSP_I2C_NUM, .addr = ES7210_CODEC_DEFAULT_ADDR,
                                      .bus_handle = bsp_i2c_get_handle() };
    if (!i2c_cfg.bus_handle) { e = ESP_ERR_INVALID_STATE; goto fail; }
    s_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!s_ctrl) { e = ESP_FAIL; goto fail; }

    step = "es7210";                       /* es7210_codec_new resets and configures the chip over I2C */
    es7210_codec_cfg_t es_cfg = { .ctrl_if = s_ctrl, .master_mode = false, .mic_selected = MIC_SELECT };
    s_codec = es7210_codec_new(&es_cfg);
    if (!s_codec) { e = ESP_FAIL; goto fail; }

    step = "codec dev";
    esp_codec_dev_cfg_t dev_cfg = { .dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = s_codec, .data_if = s_data };
    s_dev = esp_codec_dev_new(&dev_cfg);
    if (!s_dev) { e = ESP_ERR_NO_MEM; goto fail; }

    step = "codec open";                   /* reconfigures the slots, enables the channel, then the ES7210 */
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) | ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = (uint32_t)cfg->rate,
    };
    if (esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) { e = ESP_FAIL; goto fail; }
    s_dev_opened = true;

    step = "gain";
    if (esp_codec_dev_set_in_gain(s_dev, (float)cfg->gain_db) != ESP_CODEC_DEV_OK) { e = ESP_FAIL; goto fail; }

    s_skip = (size_t)cfg->rate * MIC_SETTLE_MS / 1000;
    s_open = true;
    ESP_LOGI(TAG, "open: %d Hz, gain %d dB, i2s %d", cfg->rate, cfg->gain_db, CONFIG_BSP_I2S_NUM);
    return ESP_OK;

fail:
    ESP_LOGW(TAG, "open failed at %s: %s", step, esp_err_to_name(e));
    teardown();
    return e;
}

int mic_read(int16_t *buf, size_t frames, int timeout_ms)
{
    if (!s_open || !buf || frames == 0) return -1;
    while (s_skip > 0) {                  /* the settling pop, read into the caller's buffer and dropped */
        size_t want = s_skip < frames ? s_skip : frames, g = 0;
        esp_err_t e = i2s_channel_read(s_rx, buf, want * 2 * sizeof(int16_t), &g, pdMS_TO_TICKS(MIC_SETTLE_MS + 100));
        if (e != ESP_OK && e != ESP_ERR_TIMEOUT) return -1;
        if (g == 0) return -1;
        size_t n = g / (2 * sizeof(int16_t));
        s_skip = n < s_skip ? s_skip - n : 0;
    }
    size_t got = 0;
    esp_err_t e = i2s_channel_read(s_rx, buf, frames * 2 * sizeof(int16_t), &got,
                                   pdMS_TO_TICKS(timeout_ms > 0 ? timeout_ms : 1));
    size_t n = got / (2 * sizeof(int16_t));
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) return -1;
    if (n == 0) return -1;
    return (int)n;
}

void mic_close(void)
{
    bool was = s_open;
    s_open = false;
    teardown();
    if (was) ESP_LOGI(TAG, "closed");
}

bool mic_is_open(void) { return s_open; }
