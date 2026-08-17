#pragma once

#include "esp_err.h"
#include "modem.h"
#include "spectrum.h"
#include "transport.h"

typedef struct {
    char text[1024];
    size_t text_len;
    int menu_open;
    modem_mode_t selected_mode;
    psk_mode_t selected_psk_mode;
    transport_mode_t transport_mode;
    int rx_tone_hz;
} display_state_t;

esp_err_t display_init(void);
void display_append_text(display_state_t *state, const char *text);
void display_render(const display_state_t *state, const spectrum_t *spectrum);
void display_set_menu_open(int open);
int display_take_mode_selection(modem_mode_t *mode);
int display_take_psk_mode_selection(psk_mode_t *mode);
int display_take_transport_selection(transport_mode_t *mode);
int display_take_tone_selection(int *tone_hz);
void display_task_handler(void);
