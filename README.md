# ESP32-S3 多模式数字解调器

这是一个 ESP-IDF 项目，用 ESP32-S3 开发板实现音频输入数字模式接收和解码。当前目标是把 BPSK/PSK31/63、FT8/FT4、RTTY、CW、低速 MFSK16 统一到同一套 modem 解码框架中，并在屏幕上显示频谱 / 瀑布图和解码文本。

更详细的架构图和模块划分见 [arch.md](arch.md)。

## 当前功能

已接入的调制方式：

- BPSK31
- PSK63
- FT8
- FT4
- RTTY 45.45 baud / 170 Hz shift
- CW
- MFSK16

主要功能：

- 统一解码入口：所有音频都进入 `modem_process()` / `decoder_layer_feed()`。
- 小组件式解码：按 PSK、FSK、FT、CW、MFSK 家族选择后端。
- FT8/FT4：集成 `components/ft8lib`，包含候选搜索、LDPC、CRC 和消息解码。
- PSK/RTTY：旧 PSK/RTTY 代码已重命名为 `components/psk_rtty`，不再保留 `nuepsk` 名称。
- CW：基于音调能量、mark/space 时长和 Morse 表输出字符。
- MFSK16：包含 16 音调能量、Gray 反映射、soft bit、反交织、Viterbi 和 varicode。
- 设置界面：通过用户按键进入 / 退出设置，通过触摸选择调制方式和输出方式。
- 输出方式：USB 串口、WiFi、BT 三选一，输出为解码后的原始二进制字节流。
- 显示：频谱 / 瀑布图和解码文本；触摸瀑布图可调整当前接收中心频点。

## 架构概览

```text
ES7210/I2S audio
    -> audio_input.c
    -> modem.c
    -> decoder_layer.c
    -> mode backend
    -> decoded bytes
    -> transport.c
```

核心文件：

- `main/audio_input.c`：音频输入、采样和抽取。
- `main/modem.c`：公开 modem API、模式切换、频点设置和输出缓冲。
- `main/decoder_layer.c`：统一解码层，按模式 family 分发到后端。
- `main/demod_primitives.c`：模式 profile、音调限制、tone power 等基础 DSP 函数。
- `components/psk_rtty`：BPSK31、PSK63、RTTY 后端。
- `components/ft8lib`：FT8 / FT4 后端。
- `main/transport.c`：USB / WiFi / BT 输出选择。
- `main/display.c`、`main/ui.c`：屏幕显示、瀑布图、触摸和设置菜单。

模式参数集中在 `demod_profile_t`，同一份 profile 同时用于：

- 解码后端选择
- 符号率和音调配置
- 瀑布图带宽
- 触摸调谐范围

## 用户操作

- 按一次用户按键：进入设置界面。
- 再按一次用户按键：退出设置界面。
- 在设置界面触摸选择调制模式。
- 在设置界面触摸选择输出方式：USB、WiFi 或 BT。
- 在瀑布图区域触摸：切换当前模式的接收中心频点。

## 传输方式

`transport.c` 统一负责解码字节输出。三种方式互斥，不能同时启用：

- USB 串口：通过 USB serial 输出解码字节。
- WiFi：通过当前 WiFi 后端发送解码字节。
- BT：已作为模式选择入口保留，后续需要按产品要求补齐 BLE/SPP 具体实现。

注意：传输层处理的是原始二进制字节流，不应按 NUL 结尾字符串处理。

## 硬件默认配置

引脚配置保存在 `main/board_config.h`。如果开发板、LCD、触摸控制器或音频 codec 接线不同，需要修改该文件。

默认音频配置：

- Codec 输入采样率：`BOARD_CODEC_SAMPLE_RATE_HZ = 16000`
- Modem 处理采样率：`BOARD_MODEM_SAMPLE_RATE_HZ = 8000`
- I2S：MCLK GPIO38，BCLK GPIO14，WS GPIO13，DOUT GPIO45，DIN GPIO12
- ES7210 I2C 地址：`0x41`
- ES7210 ADC 增益：`24 dB`

默认屏幕和触摸配置：

- LCD：320x240，SPI3
- MOSI：GPIO40
- SCLK：GPIO41
- DC：GPIO39
- 背光：GPIO42
- 触摸：FT6336 / FT5x06，I2C SDA GPIO1，SCL GPIO2
- 用户按键 SW1：GPIO0

## 构建

在 ESP-IDF v6.0.2 PowerShell 环境中构建：

```powershell
C:\WINDOWS\System32\WindowsPowerShell\v1.0\powershell.exe -NoExit -ExecutionPolicy Bypass -NoProfile -Command "& {. 'C:\Espressif\tools\Microsoft.v6.0.2.PowerShell_profile.ps1'}"
idf.py set-target esp32s3
idf.py build
```

或者在已加载 ESP-IDF 环境后直接执行：

```powershell
idf.py build
```

## 烧录

```powershell
idf.py -p COMx flash monitor
```

将 `COMx` 替换为实际串口号。

## 分区

项目使用自定义 `partitions.csv`，factory app 分区为 2 MB。当前固件包含 FT8/FT4 解码库，默认分区配置已留出足够空间。

## 参考

- `arch.md`：当前统一解码层和模块划分说明。
- `components/ft8lib`：FT8/FT4 轻量 C 解码后端。
- `components/psk_rtty`：PSK/RTTY 后端。
- Fldigi 和 uSDX 可作为算法参考，但不要直接整块复制桌面端 GPL 实现，除非明确接受相应许可证义务。
