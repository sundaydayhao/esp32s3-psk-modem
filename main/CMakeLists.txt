idf_component_register(
    SRCS
        "app_main.c"
        "audio_decim.c"
        "audio_input.c"
        "board_i2c.c"
        "decoder_layer.c"
        "demod_primitives.c"
        "display.c"
        "modem.c"
        "psk_modes.c"
        "spectrum.c"
        "transport.c"
        "ui.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_timer esp_driver_spi esp_driver_i2c esp_driver_i2s esp_driver_gpio esp_driver_usb_serial_jtag esp_wifi esp_netif nvs_flash lvgl psk_rtty ft8lib espressif__esp_lcd_touch_ft5x06
)
