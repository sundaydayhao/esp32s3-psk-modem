# ESP32-S3 音频 PSK 接收机

参考 `C:\projects\hpm6750-psk-modem` 的 PSK 接收实现，使用 ES7210/I2S 音频输入、LCD 频谱/文本和常驻 Wi-Fi AP。固件为 ESP-IDF 5.5.5、ESP32-S3、8 MB Flash、Octal PSRAM 配置。

## 速率结论（2026-09-07）

建议最高上板验证目标为 **8PSK1200F：1230.77 Baud，3692.31 bit/s 调制码流，K=7、2/3 码率纠错后 2461.54 bit/s**。最后一个数尚未扣除 varicode、前导、同步和帧开销，不能直接当作文本文件吞吐量。

23 个非实验在线 PSK 模式通过完整 168 字节正文的无噪声主机回归；8PSK1200F 另通过 10 组不同正文、合计 1680 字节的回归。测试显式包含正文外的获取/排空保护文字，比较正文时不做空白归一化、不接受部分片段匹配。

**开发板和音频线未连接，尚未烧录，也未测出板端无误码上限。** CPU、Wi-Fi 并发、ES7210 前端和信道仍需实测。8PSK1000（3000 bit/s 无纠错）和双载波 2x8PSK1200F（理论纠错后 4923.08 bit/s）在部分正文测试中仍有错误，页面标为实验模式。8PSK125F/250F/500F 因 K=16/16/13 高状态数开销，在 S3 在线配置中禁用。

详细依据和板测验收步骤见 [速率评估](docs/RATE_ASSESSMENT.md)。

## 手机查看

1. 烧录并启动开发板。
2. 手机连接 Wi-Fi **PSK-MODEM**，密码 **pskmodem123**。
3. 浏览器打开 **http://192.168.4.1/**。手机提示没有互联网时选择保持连接。
4. 网页选择与发送端相同的模式和中心频率。开机默认为 **BPSK500 / 1500 Hz**。

无需路由器、联网或安装 App。热点为 WPA2、信道 6，最多 4 台手机。连接后需手动打开地址；本版本不实现强制门户/DNS 自动弹页。

页面提供实时文本、HEX、保存原始 `.bin`、清空本页、模式选择、100–3500 Hz 调谐和运行统计。板端保留最近 16 KiB；每台手机有独立读取游标，本页保留最近 64 KiB。断线超过历史窗口会提示丢失。接收数据按文本呈现，不执行 HTML/脚本。文本视图按 UTF-8 展示，其他编码或二进制可用 HEX/原始保存查看。

热点始终运行，与 USB/UDP 输出选择独立。默认同时保留 USB 解码输出；USB/UDP 发送在独立任务，慢客户端不阻塞音频。选择 WiFi 传输时仍保留原 UDP 7355 广播；BT 是原有占位入口。

SW1 循环切换在线模式；USB 命令如 `@BPSK500`、`@8PSK1200F` 仍可使用。手机、串口和按键设置均经队列交给音频任务。

## 硬件与构建

引脚见 `main/board_config.h`：ES7210 `0x41`，I2C SDA GPIO1/SCL GPIO2；I2S MCLK GPIO38、BCLK GPIO14、WS GPIO13、DIN GPIO12；LCD 320×240，SW1 GPIO0。Codec 和解码采样率均为 **16000 Hz**，输入通道 0。保持已有音频增益寄存器配置，实际信号应避免削顶。

```powershell
. C:\Espressif\tools\Microsoft.v5.5.5.PowerShell_profile.ps1
Set-Location C:\projects\esp32s3-psk-modem
idf.py build
idf.py -p COMx flash monitor
```

将 `COMx` 换成实际串口。固件为 `build/psk_modem_s3.bin`；完整烧录还需要 `build/bootloader/bootloader.bin` 和 `build/partition_table/partition-table.bin`，`idf.py flash` 会处理正确地址。

CPU 固定 240 MHz、性能优化编译；音频/I2S 在 core 1，Wi-Fi、TCP/IP、HTTP 和 UI 在 core 0。Octal PSRAM 是此固件的硬件要求：瀑布图和 MFSK 大缓存放到外部 RAM，PSK 热路径、DMA 和任务栈保留片内空间。

## 验证

```powershell
./tools/build_host_tools.ps1
C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe tools/strict_rx_regression.py --guard-text --out build/strict-release
C:\Espressif\tools\python\v5.5.5\venv\Scripts\python.exe tools/rate_payload_regression.py
./tools/run_support_tests.ps1
```

正文测试调用参考项目的 `build/host-tools/host_tx_wav.exe`，再通过本项目实际 FIR/DC 和 PSK C 解码器，采用与固件相同的 K≤7 存储配置及 64 样本调用边界。结果在 `build/strict-release`、`build/rate-probes`。`--include-experimental` 包含已知失败模式并按真实结果返回失败退出码。

浏览器界面测试：`python tools/web_preview.py`，打开 `http://127.0.0.1:18763/`。该脚本提供明确标注的测试数据，不模拟 ESP32 的 Wi-Fi/HTTP 驱动。

旧的 `run_host_regression.ps1`/`hw_wav_test.py` 使用片段匹配，**不能作为零误码验收**。FT8/FT4/RTTY/CW/MFSK 保留原入口，不在本次 PSK 速率验证范围内。架构见 [arch.md](arch.md)。
