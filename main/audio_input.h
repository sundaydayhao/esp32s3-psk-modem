#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t audio_input_init(void);
size_t audio_input_read(int16_t *samples, size_t max_samples, uint32_t timeout_ms);
uint32_t audio_input_overflows(void);
uint32_t audio_input_read_errors(void);
