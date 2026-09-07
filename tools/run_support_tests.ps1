param([string]$Gcc = "C:\cygwin64\bin\gcc.exe")
$ErrorActionPreference = "Stop"
Push-Location (Join-Path $PSScriptRoot "..")
try {
    $env:PATH = "$(Split-Path $Gcc);$env:PATH"
    New-Item -ItemType Directory -Force build/host-tools | Out-Null
    & $Gcc -std=c11 -O2 -Wall -Wextra -Werror -I main tools/test_rx_history.c main/rx_history.c -o build/host-tools/test_rx_history.exe
    if ($LASTEXITCODE) { throw "history build failed" }
    & ./build/host-tools/test_rx_history.exe
    if ($LASTEXITCODE) { throw "history test failed" }
    $sources = @("tools/test_modem_reset.c", "main/modem.c", "main/decoder_layer.c", "main/demod_primitives.c",
        "components/psk_rtty/psk_rtty.c", "components/psk_rtty/psk_modes.c", "components/psk_rtty/psk_codec.c",
        "components/ft8lib/common/monitor.c")
    $sources += (Get-ChildItem components/ft8lib/ft8,components/ft8lib/fft -Filter '*.c').FullName
    & $Gcc -std=gnu11 -O2 -D_GNU_SOURCE -DPSK_RTTY_MAX_FEC_K=7 -I main -I components/psk_rtty/include -I components/ft8lib -I components/ft8lib/ft8 -I components/ft8lib/fft @sources -lm -o build/host-tools/test_modem_reset.exe
    if ($LASTEXITCODE) { throw "modem reset build failed" }
    & ./build/host-tools/test_modem_reset.exe
    if ($LASTEXITCODE) { throw "modem reset test failed" }
} finally { Pop-Location }
