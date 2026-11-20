/* 麦克风采集：沿用 xiaozhi 该板 BoxAudioCodec 的初始化顺序
 *  - I2S 双工：TX(标准) + RX(TDM 4 槽)，16kHz
 *  - ES8311(DAC) 也初始化，保证 MCLK/PA 通路与官方一致
 *  - ES7210(ADC) 选 4 麦，输入增益 30dB
 */
#include <string.h>
#include "esp_log.h"
#include "esp_attr.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "es7210_adc.h"
#include "es8311_codec.h"
#include "esp_codec_dev_os.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "board.h"
#include "display.h"
#include "mic.h"

static const char *TAG = "mic";

static i2s_chan_handle_t s_tx;
static i2s_chan_handle_t s_rx;
static esp_codec_dev_handle_t s_in;
esp_codec_dev_handle_t mic_input_dev(void) { return s_in; }
static esp_codec_dev_handle_t s_out;
static volatile uint32_t s_rx_overflows;

static bool IRAM_ATTR rx_queue_overflow(i2s_chan_handle_t handle, i2s_event_data_t *event, void *arg)
{
    s_rx_overflows++;
    return false;
}

void mic_log_diagnostics(void)
{
    ESP_LOGI(TAG,"health rx_dma_queue_overflows=%lu",(unsigned long)s_rx_overflows);
}

esp_err_t mic_init(void)
{
    /* 1. 双工通道：TX 标准 + RX TDM，与官方一致 */
    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 12, /* 180ms RX backlog; tolerate short Flash/TLS stalls */
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx, &s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MIC_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = I2S_STD_SLOT_BOTH,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = true,
            .big_endian = false,
            .bit_order_lsb = false,
        },
        .gpio_cfg = {
            .mclk = AUDIO_MCLK,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_WS,
            .dout = AUDIO_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .sample_rate_hz = MIC_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
            .bclk_div = 8,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_STEREO,
            .slot_mask = (i2s_tdm_slot_mask_t)(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 |
                                               I2S_TDM_SLOT2 | I2S_TDM_SLOT3),
            .ws_width = I2S_TDM_AUTO_WS_WIDTH,
            .ws_pol = false,
            .bit_shift = true,
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
            .skip_mask = false,
            .total_slot = I2S_TDM_AUTO_SLOT_NUM,
        },
        .gpio_cfg = {
            .mclk = AUDIO_MCLK,
            .bclk = AUDIO_BCLK,
            .ws = AUDIO_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_DIN,
            .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_tdm_mode(s_rx, &tdm_cfg));
    i2s_event_callbacks_t callbacks={.on_recv_q_ovf=rx_queue_overflow};
    ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_rx,&callbacks,NULL));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx));
    ESP_LOGI(TAG, "duplex channels ready");

    /* 2. 数据/控制接口 */
    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = I2S_NUM_0,
        .rx_handle = s_rx,
        .tx_handle = s_tx,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!data_if) return ESP_FAIL;

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = I2C_NUM_0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = display_i2c_bus(),
    };
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!out_ctrl) return ESP_FAIL;
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    if (!gpio_if) return ESP_FAIL;

    /* 3. ES8311（DAC）——与官方一样初始化，保证时钟与 PA 通路 */
    es8311_codec_cfg_t es8311_cfg = {0};
    es8311_cfg.ctrl_if = out_ctrl;
    es8311_cfg.gpio_if = gpio_if;
    es8311_cfg.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es8311_cfg.pa_pin = AUDIO_PA;
    es8311_cfg.use_mclk = true;
    es8311_cfg.hw_gain.pa_voltage = 5.0;
    es8311_cfg.hw_gain.codec_dac_voltage = 3.3;
    const audio_codec_if_t *out_codec = es8311_codec_new(&es8311_cfg);
    if (!out_codec) return ESP_FAIL;

    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = out_codec,
        .data_if = data_if,
    };
    s_out = esp_codec_dev_new(&dev_cfg);
    if (!s_out) return ESP_FAIL;

    /* 4. ES7210（ADC） */
    i2c_cfg.addr = ES7210_CODEC_DEFAULT_ADDR;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (!in_ctrl) return ESP_FAIL;

    es7210_codec_cfg_t es7210_cfg = {0};
    es7210_cfg.ctrl_if = in_ctrl;
    es7210_cfg.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 |
                              ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    const audio_codec_if_t *in_codec = es7210_codec_new(&es7210_cfg);
    if (!in_codec) return ESP_FAIL;

    dev_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN;
    dev_cfg.codec_if = in_codec;
    s_in = esp_codec_dev_new(&dev_cfg);
    if (!s_in) return ESP_FAIL;

    /* 5. 输出是标准双声道；输入必须按 ES7210 的 4-slot TDM 描述。
     * 若输入写成 channel=2 且 mask=0，驱动会在 TDM 路径把 16-bit
     * 错误折半为 8-bit，I2S 仍按 16-bit 读取，直接造成失真和底噪。 */
    esp_codec_dev_sample_info_t out_fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = MIC_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_out, &out_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "out open failed");
    }
    esp_codec_dev_sample_info_t in_fs = {
        .bits_per_sample = 16,
        .channel = 4,
        .channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0) |
                        ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1),
        .sample_rate = MIC_SAMPLE_RATE,
    };
    if (esp_codec_dev_open(s_in, &in_fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "in open failed");
        return ESP_FAIL;
    }
    if (esp_codec_dev_set_in_channel_gain(
            s_in, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), 30.0) != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "main mic gain setup failed");
    }
    esp_codec_dev_set_out_vol(s_out, 0);   /* 静音喇叭，避免回声 */

    ESP_LOGI(TAG, "mic ready: %d Hz / TDM slots 0+1, main gain 30dB", MIC_SAMPLE_RATE);
    return ESP_OK;
}

/* 播放音量（0=静音；问答播放时临时调大） */
void mic_set_playback_vol(int vol)
{
    if (s_out) esp_codec_dev_set_out_vol(s_out, vol);
}

int mic_play(const void *buf, int len)
{
    if (!s_out) return -1;
    if (esp_codec_dev_write(s_out, (void *)buf, len) != ESP_CODEC_DEV_OK) return -1;
    return len;
}

int mic_read(void *buf, int len)
{
    if (!s_rx) return -1;
    /* 直接读 I2S，带超时：避免 codec 掉电时永久阻塞把录音任务卡死 */
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx, buf, len, &got, pdMS_TO_TICKS(300));
    if (err != ESP_OK) return 0;
    return (int)got;
}
