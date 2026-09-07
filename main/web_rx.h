#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "ui.h"

typedef struct {
    modem_mode_t mode;
    psk_mode_t psk_mode;
    int tone_hz;
    uint32_t rms;
    uint32_t clips;
    uint32_t sample_rate;
    uint32_t work_us;
    uint32_t budget_us;
    uint32_t peak_work_us;
    uint32_t overruns;
    uint32_t dma_overflows;
    uint32_t read_errors;
    uint32_t output_drops;
    uint32_t decode_drops;
    uint32_t lcd_drops;
    uint32_t lock_state;
} web_rx_status_t;

esp_err_t web_rx_start(void);
void web_rx_publish(const uint8_t *data, size_t len);
void web_rx_update_status(const web_rx_status_t *status);
bool web_rx_take_control(ui_event_t *event);
