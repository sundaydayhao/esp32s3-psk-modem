# ESP32-S3 PSK 调制解调器

这是一个 ESP-IDF 工程，用于把 ESP32-S3 开发板作为音频输入的数字模式接收机。

固件围绕 `nuepsk_source` 中已有的模式组织：

- BPSK31
- QPSK31 USB
- QPSK31 LSB / reverse
- RTTY 45.45 baud, 170 Hz shift
- PSK63

旧版 NUE-PSK 源码面向 dsPIC，依赖 dsPIC 寄存器、存储属性以及 Microchip DSP
库类型。本工程保留 NUE-PSK 作为模式覆盖、RTTY 行为、菜单和接收流程的主要参考来源，
不使用第三方 PSK 调制解调库。

## DSP 策略

`components/nuepsk` 是 `nuepsk_source` 的 ESP-IDF 移植层。当前模式来自 NUE
固件定义：

- BPSK31
- QPSK31 USB
- QPSK31 LSB / reverse
- RTTY 45.45 baud, 170 Hz shift
- PSK63

该组件已经使用 NUE 的 Varicode 表和 RTTY Baudot 表。剩余移植工作的重点是把 FIR、
AFC、符号同步和 QPSK Viterbi 逻辑从 `nuepsk_source/C Source/PskDet.c` 中迁移出来，
同时用可移植的 ESP32 代码替换 dsPIC 专用的 `fractional`、`FIRDecimate`、
`FFTComplexIP`、寄存器访问和 LCD 调用。

## 界面

- 上半部分：音频频谱，以当前选择的接收音调为中心。
- 下半部分：解码后的文本流。
- SW1：打开或关闭模式菜单。
- 菜单触摸：选择调制模式。
- 频谱触摸：修改接收音调。

## 硬件默认配置

引脚分配保存在 `main/board_config.h`。这些配置必须和你的 ESP32-S3 开发板、LCD、
触摸控制器以及音频编解码器接线一致。

如果你的板卡版本不同，请修改 `main/board_config.h`：

- `BOARD_SW1_GPIO`
- `BOARD_LCD_*`
- `BOARD_TOUCH_*`
- `BOARD_AUDIO_*`

当前显示和触摸默认配置：

- LCD：ST7789，320x240，SPI3，SCLK GPIO41，MOSI GPIO40，DC GPIO39，背光 GPIO42。
- Touch：FT6336/FT5x06，I2C SDA GPIO1，SCL GPIO2。
- SW1：GPIO0。

## 构建

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'}"
idf.py set-target esp32s3
idf.py build
```
