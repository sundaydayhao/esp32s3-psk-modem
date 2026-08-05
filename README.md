# ESP32-S3 PSK Modem

ESP-IDF project for an ESP32-S3 board used as an audio-input digital-mode
receiver.

The firmware is structured around the modes present in `nuepsk_source`:

- BPSK31
- QPSK31 USB
- QPSK31 LSB / reverse
- RTTY 45.45 baud, 170 Hz shift
- PSK63

The old NUE-PSK source is dsPIC-oriented and depends on dsPIC registers,
memory attributes, and Microchip DSP library types. This project keeps
NUE-PSK as the single source for mode coverage, RTTY behavior, menus, and
receiver workflow. No third-party PSK modem library is used.

## DSP Strategy

`components/nuepsk` is the ESP-IDF porting layer for `nuepsk_source`.
Current modes are the modes defined by the NUE firmware:

- BPSK31
- QPSK31 USB
- QPSK31 LSB / reverse
- RTTY 45.45 baud, 170 Hz shift
- PSK63

The component already uses NUE's Varicode table and RTTY Baudot table. The
remaining porting work is to move the FIR, AFC, symbol sync, and QPSK Viterbi
logic out of `nuepsk_source/C Source/PskDet.c` while replacing dsPIC-only
`fractional`, `FIRDecimate`, `FFTComplexIP`, register, and LCD calls with
portable ESP32 code.

## UI

- Top half: audio spectrum, centered around the selected receive tone.
- Bottom half: decoded text stream.
- SW1: open/close mode menu.
- Touch in menu: select modulation mode.
- Touch on spectrum: change receive tone.

## Hardware Defaults

Pin assignments are kept in `main/board_config.h`. They must match your
ESP32-S3 board, LCD, touch controller, and audio codec wiring.

Edit `main/board_config.h` if your board revision is different:

- `BOARD_AUDIO_ADC_CHANNEL`
- `BOARD_SW1_GPIO`
- `BOARD_LCD_*`
- `BOARD_TOUCH_*`

Current display/touch defaults:

- LCD: ST7789, 240x320, SPI2, SCLK GPIO3, MOSI GPIO5, CS GPIO4, DC GPIO6,
  backlight GPIO2.
- Touch: FT6336/FT5x06, I2C SDA GPIO0, SCL GPIO1.
- SW1: GPIO9.

## Build

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'}"
idf.py set-target esp32s3
idf.py build
```
