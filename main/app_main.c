#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "audio_input.h"
#include "board_config.h"
#include "display.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "modem.h"
#include "spectrum.h"
#include "transport.h"
#include "ui.h"
#include "web_rx.h"

static const char *TAG = "psk-modem";


typedef struct {
    char text[96];
    size_t len;
} decoded_msg_t;

typedef struct {
    int16_t samples[SPECTRUM_MAX_SAMPLES];
    size_t count;
} spectrum_msg_t;

static uint32_t s_lcd_drops;
static QueueHandle_t s_decoded_queue;
static QueueHandle_t s_control_queue;
static QueueHandle_t s_spectrum_queue;
static modem_t s_audio_modem;
static int16_t s_audio_samples[256];
static char s_audio_decoded[sizeof(((decoded_msg_t *)0)->text)];
static spectrum_msg_t s_audio_spectrum_msg;

static int32_t calc_block_rms(const int16_t *samples, size_t count)
{
    if (count == 0) {
        return 0;
    }
    int64_t sum_sq = 0;
    for (size_t i = 0; i < count; ++i) {
        sum_sq += (int32_t)samples[i] * (int32_t)samples[i];
    }
    return (int32_t)sqrtf((float)sum_sq / (float)count);
}

static void publish_decoded_text(const char *decoded, size_t decoded_len)
{
    if (decoded_len == 0) {
        return;
    }

    transport_send((const uint8_t *)decoded, decoded_len);

    decoded_msg_t msg = {0};
    msg.len = decoded_len;
    const size_t shown = decoded_len < sizeof(msg.text) - 1 ? decoded_len : sizeof(msg.text) - 1;
    for (size_t i = 0; i < shown; ++i) {
        const unsigned char ch = (unsigned char)decoded[i];
        msg.text[i] = (ch >= 0x20 && ch < 0x7f) || ch == '\r' || ch == '\n' ? (char)ch : '.';
    }
    msg.text[shown] = '\0';
    if (xQueueSend(s_decoded_queue, &msg, 0) != pdTRUE) s_lcd_drops += decoded_len;
}

static void apply_control_event(modem_t *modem, const ui_event_t *event)
{
    switch (event->type) {
    case UI_EVENT_MODE_CHANGED:
        modem_set_mode(modem, event->mode);
        break;
    case UI_EVENT_PSK_MODE_CHANGED:
        modem_set_psk_mode(modem, event->psk_mode);
        break;
    case UI_EVENT_TRANSPORT_CHANGED:
        transport_set_mode(event->transport_mode);
        break;
    case UI_EVENT_TONE_CHANGED:
        modem_set_rx_tone(modem, event->tone_hz);
        break;
    case UI_EVENT_NONE:
    default:
        break;
    }
}

static void audio_task(void *arg)
{
    /* Allocate/configure I2S on core 1, so its interrupt follows the DSP owner. */
    ESP_ERROR_CHECK(audio_input_init());
    const modem_config_t config = {
        .mode = MODEM_MODE_PSK63,
        .psk_mode = BOARD_DEFAULT_PSK_MODE,
        .sample_rate_hz = BOARD_MODEM_SAMPLE_RATE_HZ,
        .rx_tone_hz = BOARD_DEFAULT_TONE_HZ,
        .squelch = 30,
    };
    modem_t *modem = &s_audio_modem;
    modem_init(modem, &config);
    web_rx_status_t status = {0};
    int64_t window_start = esp_timer_get_time();
    uint32_t window_samples = 0, window_work_us = 0, window_budget_us = 0;
    uint32_t last_overflows = audio_input_overflows();

    while (true) {
        ui_event_t event;
        while (xQueueReceive(s_control_queue, &event, 0) == pdTRUE) {
            apply_control_event(modem, &event);
        }
        size_t count = audio_input_read(s_audio_samples, 256, 50);
        int64_t work_start = esp_timer_get_time();
        uint32_t overflows = audio_input_overflows();
        if (overflows != last_overflows) {
            /* Missing PCM breaks timing/FEC history: reacquire explicitly. */
            modem_set_mode(modem, modem_get_mode(modem));
            last_overflows = overflows;
        }
        if (count) {
            s_audio_spectrum_msg.count = count;
            memcpy(s_audio_spectrum_msg.samples, s_audio_samples, count * sizeof(int16_t));
            xQueueOverwrite(s_spectrum_queue, &s_audio_spectrum_msg);
            status.rms = calc_block_rms(s_audio_samples, count);
            for (size_t i = 0; i < count; ++i)
                if (s_audio_samples[i] >= 32766 || s_audio_samples[i] <= -32767) ++status.clips;
            /* Feed every sample, including silence. An RMS gate must not discard
             * symbol envelopes or reset long interleavers between quiet symbols. */
            for (size_t pos = 0; pos < count; pos += 64) {
                size_t n = count - pos;
                if (n > 64) n = 64;
                size_t len = modem_process(modem, s_audio_samples + pos, n,
                                           s_audio_decoded, sizeof(s_audio_decoded));
                publish_decoded_text(s_audio_decoded, len);
            }
            uint32_t work_us = (uint32_t)(esp_timer_get_time() - work_start);
            uint32_t budget_us = count * 1000000U / BOARD_MODEM_SAMPLE_RATE_HZ;
            /* One tick per 16 ms block lets IDLE1 run even during overload.
             * Include this reserved tick in deadline/load accounting. */
            if (work_us + portTICK_PERIOD_MS * 1000U >= budget_us) ++status.overruns;
            if (work_us > status.peak_work_us) status.peak_work_us = work_us;
            window_samples += count;
            window_work_us += work_us + portTICK_PERIOD_MS * 1000U;
            window_budget_us += budget_us;
        }
        int64_t now = esp_timer_get_time();
        if (now - window_start >= 1000000) {
            status.mode = modem_get_mode(modem);
            status.psk_mode = modem_get_psk_mode(modem);
            status.tone_hz = modem_get_rx_tone(modem);
            status.sample_rate = (uint32_t)((uint64_t)window_samples * 1000000U / (now - window_start));
            status.work_us = window_work_us;
            status.budget_us = window_budget_us;
            status.dma_overflows = overflows;
            status.read_errors = audio_input_read_errors();
            status.output_drops = transport_output_drops();
            status.decode_drops = modem->output_dropped_bytes;
            status.lcd_drops = s_lcd_drops;
            status.lock_state = modem->decoder.psk_rtty_rx.lock_state;
            web_rx_update_status(&status);
            window_start = now;
            window_samples = window_work_us = window_budget_us = 0;
        }
        vTaskDelay(1);
    }
}

static void ui_task(void *arg)
{
    display_state_t display_state = {
        .selected_mode = MODEM_MODE_PSK63,
        .selected_psk_mode = BOARD_DEFAULT_PSK_MODE,
        .transport_mode = TRANSPORT_MODE_USB_SERIAL,
        .rx_tone_hz = BOARD_DEFAULT_TONE_HZ,
    };
    spectrum_t spectrum = {0};
    int64_t last_render_us = 0;

    ESP_LOGI(TAG, "ui task started");

    while (true) {
        decoded_msg_t msg;
        while (xQueueReceive(s_decoded_queue, &msg, 0) == pdTRUE) {
            display_append_text(&display_state, msg.text);
        }

        const ui_event_t event = ui_poll(&display_state);
        switch (event.type) {
        case UI_EVENT_MODE_CHANGED:
            display_state.selected_mode = event.mode;
            if (event.mode == MODEM_MODE_BPSK31) {
                display_state.selected_psk_mode = PSK_MODE_BPSK31;
            } else if (event.mode == MODEM_MODE_PSK63 && display_state.selected_psk_mode == PSK_MODE_BPSK31) {
                display_state.selected_psk_mode = PSK_MODE_BPSK63;
            }
            if (display_state.rx_tone_hz < modem_mode_band_min_hz(event.mode) ||
                display_state.rx_tone_hz > modem_mode_band_max_hz(event.mode)) {
                display_state.rx_tone_hz = (modem_mode_band_min_hz(event.mode) +
                                           modem_mode_band_max_hz(event.mode)) / 2;
                ui_event_t tone_event = {
                    .type = UI_EVENT_TONE_CHANGED,
                    .tone_hz = display_state.rx_tone_hz,
                };
                (void)xQueueSend(s_control_queue, &tone_event, portMAX_DELAY);
            }
            (void)xQueueSend(s_control_queue, &event, portMAX_DELAY);
            break;
        case UI_EVENT_PSK_MODE_CHANGED:
            display_state.selected_psk_mode = event.psk_mode;
            display_state.selected_mode = event.psk_mode == PSK_MODE_BPSK31 ? MODEM_MODE_BPSK31 : MODEM_MODE_PSK63;
            (void)xQueueSend(s_control_queue, &event, portMAX_DELAY);
            break;
        case UI_EVENT_TRANSPORT_CHANGED:
            display_state.transport_mode = event.transport_mode;
            (void)xQueueSend(s_control_queue, &event, portMAX_DELAY);
            break;
        case UI_EVENT_TONE_CHANGED:
            display_state.rx_tone_hz = event.tone_hz;
            (void)xQueueSend(s_control_queue, &event, portMAX_DELAY);
            break;
        case UI_EVENT_NONE:
        default:
            break;
        }

        const int64_t now_us = esp_timer_get_time();
        if (now_us - last_render_us >= 100000) {
            last_render_us = now_us;
            spectrum_msg_t spectrum_msg;
            if (xQueueReceive(s_spectrum_queue, &spectrum_msg, 0) == pdTRUE) {
                spectrum_analyze(spectrum_msg.samples, spectrum_msg.count, BOARD_MODEM_SAMPLE_RATE_HZ, &spectrum);
            }
            display_render(&display_state, &spectrum);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(display_init());
    ESP_ERROR_CHECK(ui_init());
    ESP_ERROR_CHECK(transport_init(TRANSPORT_MODE_USB_SERIAL));
    s_decoded_queue = xQueueCreate(8, sizeof(decoded_msg_t));
    s_control_queue = xQueueCreate(4, sizeof(ui_event_t));
    s_spectrum_queue = xQueueCreate(1, sizeof(spectrum_msg_t));
    ESP_ERROR_CHECK(s_decoded_queue && s_control_queue && s_spectrum_queue ? ESP_OK : ESP_ERR_NO_MEM);

    BaseType_t task_ok = xTaskCreatePinnedToCore(audio_task, "psk_audio", 16384, NULL, 8, NULL, 1);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);
    task_ok = xTaskCreatePinnedToCore(ui_task, "psk_ui", 6144, NULL, 2, NULL, 0);
    ESP_ERROR_CHECK(task_ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    ESP_LOGI(TAG, "ESP32-S3 PSK modem started");

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
