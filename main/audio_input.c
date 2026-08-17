#include "audio_input.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "audio_decim.h"
#include "board_config.h"
#include "board_i2c.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "audio";
static i2s_chan_handle_t s_rx_handle;
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_es7210_dev;
static audio_decim_state_t s_decim_state;
static int16_t s_raw_samples[1024];
static int16_t s_mono_samples[512];

#define ES7210_RESET_REG00            0x00
#define ES7210_MAINCLK_REG02          0x02
#define ES7210_LRCK_DIVH_REG04        0x04
#define ES7210_LRCK_DIVL_REG05        0x05
#define ES7210_POWER_DOWN_REG06       0x06
#define ES7210_OSR_REG07              0x07
#define ES7210_TIME_CONTROL0_REG09    0x09
#define ES7210_TIME_CONTROL1_REG0A    0x0A
#define ES7210_SDP_INTERFACE1_REG11   0x11
#define ES7210_SDP_INTERFACE2_REG12   0x12
#define ES7210_ADC12_HPF1_REG23       0x23
#define ES7210_ADC12_HPF2_REG22       0x22
#define ES7210_ADC34_HPF1_REG21       0x21
#define ES7210_ADC34_HPF2_REG20       0x20
#define ES7210_ANALOG_REG40           0x40
#define ES7210_MIC12_BIAS_REG41       0x41
#define ES7210_MIC34_BIAS_REG42       0x42
#define ES7210_MIC1_GAIN_REG43        0x43
#define ES7210_MIC2_GAIN_REG44        0x44
#define ES7210_MIC3_GAIN_REG45        0x45
#define ES7210_MIC4_GAIN_REG46        0x46
#define ES7210_MIC1_POWER_REG47       0x47
#define ES7210_MIC2_POWER_REG48       0x48
#define ES7210_MIC3_POWER_REG49       0x49
#define ES7210_MIC4_POWER_REG4A       0x4A
#define ES7210_MIC12_POWER_REG4B      0x4B
#define ES7210_MIC34_POWER_REG4C      0x4C

static esp_err_t es7210_write_reg(uint8_t reg_addr, uint8_t reg_value)
{
    uint8_t write_buf[2] = { reg_addr, reg_value };
    return i2c_master_transmit(s_es7210_dev, write_buf, sizeof(write_buf), 1000);
}

static esp_err_t codec_i2c_init(void)
{
    ESP_RETURN_ON_ERROR(board_i2c_get_bus(&s_i2c_bus), TAG, "i2c bus");

    if (!s_es7210_dev) {
        const i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = BOARD_ES7210_I2C_ADDR,
            .scl_speed_hz = BOARD_I2C_SPEED_HZ,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_es7210_dev), TAG, "es7210 add device");
    }

    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_RESET_REG00, 0xFF), TAG, "es7210 reset 1");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_RESET_REG00, 0x32), TAG, "es7210 reset 2");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_TIME_CONTROL0_REG09, 0x30), TAG, "es7210 time0");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_TIME_CONTROL1_REG0A, 0x30), TAG, "es7210 time1");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_ADC12_HPF1_REG23, 0x2A), TAG, "es7210 hpf1");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_ADC12_HPF2_REG22, 0x0A), TAG, "es7210 hpf2");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_ADC34_HPF1_REG21, 0x2A), TAG, "es7210 hpf3");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_ADC34_HPF2_REG20, 0x0A), TAG, "es7210 hpf4");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, 0x60), TAG, "es7210 fmt");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x02), TAG, "es7210 tdm");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_ANALOG_REG40, 0xC3), TAG, "es7210 analog");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC12_BIAS_REG41, 0x70), TAG, "es7210 bias12");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC34_BIAS_REG42, 0x70), TAG, "es7210 bias34");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC1_GAIN_REG43, 0x1A), TAG, "es7210 gain1");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC2_GAIN_REG44, 0x1A), TAG, "es7210 gain2");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC3_GAIN_REG45, 0x1A), TAG, "es7210 gain3");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC4_GAIN_REG46, 0x1A), TAG, "es7210 gain4");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC1_POWER_REG47, 0x08), TAG, "es7210 pwr1");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC2_POWER_REG48, 0x08), TAG, "es7210 pwr2");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC3_POWER_REG49, 0x08), TAG, "es7210 pwr3");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0x08), TAG, "es7210 pwr4");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_OSR_REG07, 0x20), TAG, "es7210 osr");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MAINCLK_REG02, 0xC1), TAG, "es7210 mclk");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_LRCK_DIVH_REG04, 0x01), TAG, "es7210 lrckh");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_LRCK_DIVL_REG05, 0x00), TAG, "es7210 lrckl");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x04), TAG, "es7210 pdown");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x0F), TAG, "es7210 power12");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x0F), TAG, "es7210 power34");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_RESET_REG00, 0x71), TAG, "es7210 enable1");
    ESP_RETURN_ON_ERROR(es7210_write_reg(ES7210_RESET_REG00, 0x41), TAG, "es7210 enable2");

    ESP_LOGI(TAG, "ES7210 initialized on shared I2C bus");
    return ESP_OK;
}

static esp_err_t i2s_driver_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(BOARD_AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &s_rx_handle), TAG, "i2s channel");

    i2s_tdm_config_t tdm_cfg = {
        .clk_cfg = {
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .sample_rate_hz = BOARD_CODEC_SAMPLE_RATE_HZ,
            .mclk_multiple = BOARD_AUDIO_MCLK_MULTIPLE,
        },
        .slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO,
                                                        I2S_TDM_SLOT0 | I2S_TDM_SLOT1),
        .gpio_cfg = {
            .mclk = BOARD_AUDIO_I2S_MCLK,
            .bclk = BOARD_AUDIO_I2S_BCLK,
            .ws = BOARD_AUDIO_I2S_WS,
            .dout = GPIO_NUM_NC,
            .din = BOARD_AUDIO_I2S_DIN,
        },
    };

    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(s_rx_handle, &tdm_cfg), TAG, "i2s tdm init");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "i2s enable");
    return ESP_OK;
}

esp_err_t audio_input_init(void)
{
    audio_decim_init(&s_decim_state);
    ESP_RETURN_ON_ERROR(i2s_driver_init(), TAG, "i2s driver init");
    ESP_RETURN_ON_ERROR(codec_i2c_init(), TAG, "codec init");
    ESP_LOGI(TAG, "ES7210/I2S TDM audio backend ready: codec %d Hz, modem %d Hz",
             BOARD_CODEC_SAMPLE_RATE_HZ, BOARD_MODEM_SAMPLE_RATE_HZ);
    return ESP_OK;
}

size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms)
{
    if (!samples || max_samples == 0) {
        return 0;
    }

    const size_t decim = BOARD_CODEC_SAMPLE_RATE_HZ / BOARD_MODEM_SAMPLE_RATE_HZ;
    if (decim == 0 || BOARD_CODEC_SAMPLE_RATE_HZ % BOARD_MODEM_SAMPLE_RATE_HZ != 0) {
        ESP_LOGE(TAG, "unsupported codec/modem sample rates");
        return 0;
    }

    if (max_samples > 256) {
        max_samples = 256;
    }

    size_t bytes_read = 0;
    const size_t bytes_requested = max_samples * decim * sizeof(s_raw_samples[0]) * 2;
    const esp_err_t err = i2s_channel_read(s_rx_handle, s_raw_samples, bytes_requested, &bytes_read, timeout_ms);
    if (err != ESP_OK || bytes_read < sizeof(int16_t) * 2) {
        return 0;
    }

    const size_t frames = bytes_read / (sizeof(int16_t) * 2);
    const size_t mono_count = frames < (sizeof(s_mono_samples) / sizeof(s_mono_samples[0])) ?
                              frames : (sizeof(s_mono_samples) / sizeof(s_mono_samples[0]));
    const bool use_left = BOARD_AUDIO_INPUT_CHANNEL == 0;
    int16_t l_min = INT16_MAX;
    int16_t l_max = INT16_MIN;
    int16_t r_min = INT16_MAX;
    int16_t r_max = INT16_MIN;
    int64_t l_abs_sum = 0;
    int64_t r_abs_sum = 0;
    int64_t l_sum_sq = 0;
    int64_t r_sum_sq = 0;
    size_t l_clip_count = 0;
    size_t r_clip_count = 0;

    for (size_t i = 0; i < mono_count; ++i) {
        const int16_t left = s_raw_samples[i * 2];
        const int16_t right = s_raw_samples[i * 2 + 1];
        if (left < l_min) {
            l_min = left;
        }
        if (left > l_max) {
            l_max = left;
        }
        if (right < r_min) {
            r_min = right;
        }
        if (right > r_max) {
            r_max = right;
        }
        l_abs_sum += left < 0 ? -(int32_t)left : left;
        r_abs_sum += right < 0 ? -(int32_t)right : right;
        l_sum_sq += (int32_t)left * (int32_t)left;
        r_sum_sq += (int32_t)right * (int32_t)right;
        if (left <= INT16_MIN + 1 || left >= INT16_MAX - 1) {
            l_clip_count++;
        }
        if (right <= INT16_MIN + 1 || right >= INT16_MAX - 1) {
            r_clip_count++;
        }
        s_mono_samples[i] = use_left ? left : right;
    }

    const size_t out_count = audio_decim_process(&s_decim_state, s_mono_samples, mono_count, samples, decim);
    int16_t s_min = INT16_MAX;
    int16_t s_max = INT16_MIN;
    int64_t s_abs_sum = 0;
    int64_t s_sum_sq = 0;
    size_t s_clip_count = 0;
    for (size_t i = 0; i < out_count; ++i) {
        int32_t sample32 = (int32_t)samples[i] * BOARD_AUDIO_SOFTWARE_GAIN;
        if (sample32 > INT16_MAX) {
            sample32 = INT16_MAX;
        } else if (sample32 < INT16_MIN) {
            sample32 = INT16_MIN;
        }
        const int16_t sample = (int16_t)sample32;
        samples[i] = sample;
        if (sample < s_min) {
            s_min = sample;
        }
        if (sample > s_max) {
            s_max = sample;
        }
        s_abs_sum += sample < 0 ? -(int32_t)sample : sample;
        s_sum_sq += (int32_t)sample * (int32_t)sample;
        if (sample <= INT16_MIN + 1 || sample >= INT16_MAX - 1) {
            s_clip_count++;
        }
    }

#if BOARD_AUDIO_DEBUG_LOGS
    static int64_t last_log_us;
    const int64_t now_us = esp_timer_get_time();
    if (now_us - last_log_us >= 1000000) {
        last_log_us = now_us;
        const int32_t l_rms = mono_count ? (int32_t)sqrt((double)l_sum_sq / (double)mono_count) : 0;
        const int32_t r_rms = mono_count ? (int32_t)sqrt((double)r_sum_sq / (double)mono_count) : 0;
        const int32_t s_rms = out_count ? (int32_t)sqrt((double)s_sum_sq / (double)out_count) : 0;
        ESP_LOGI(TAG, "rx bytes=%u frames=%u ch=%c L[%d,%d] abs=%d rms=%d clip=%u R[%d,%d] abs=%d rms=%d clip=%u S[%d,%d] abs=%d rms=%d clip=%u",
                 (unsigned)bytes_read, (unsigned)frames, use_left ? 'L' : 'R',
                 l_min, l_max, mono_count ? (int)(l_abs_sum / (int64_t)mono_count) : 0, (int)l_rms, (unsigned)l_clip_count,
                 r_min, r_max, mono_count ? (int)(r_abs_sum / (int64_t)mono_count) : 0, (int)r_rms, (unsigned)r_clip_count,
                 s_min, s_max, out_count ? (int)(s_abs_sum / (int64_t)out_count) : 0, (int)s_rms, (unsigned)s_clip_count);
    }
#endif
    return out_count;
}
