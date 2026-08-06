# ESP32-S3 Multi-Mode Modem Architecture

## Goals

This project implements a touch-controlled ESP32-S3 digital modem for BPSK31,
PSK63, FT8, FT4, RTTY, CW, and low-speed MFSK16. The decoder is organized as a
single modem layer that composes small DSP and protocol blocks instead of
creating one unrelated decoder stack per mode.

The design keeps these rules:

- All receive audio enters through one modem API.
- Mode-specific behavior is selected by profiles and small protocol backends.
- The old `nuepsk` component name is removed; PSK and RTTY live under
  `psk_rtty`.
- FT8 and FT4 use a lightweight MCU-ready C backend.
- Output transport is selected as exactly one of USB serial, WiFi, or BT.
- The display uses the selected mode profile to choose waterfall bandwidth and
  touch-to-center-frequency behavior.

## Top-Level Flow

```text
ES7210/I2S audio
    -> audio_input decimation to modem sample rate
    -> modem_process()
    -> decoder_layer_feed()
    -> mode family backend
    -> decoded byte stream
    -> selected transport
    -> screen decoded text and waterfall
```

`main/modem.c` is intentionally thin. It owns the public modem state and output
buffer, then delegates all receive work to `main/decoder_layer.c`.

## Mode Profiles

Mode metadata is centralized in `main/demod_primitives.c`.

Each `demod_profile_t` describes:

- modem mode
- demodulation family
- symbol rate
- tone count
- tone spacing
- recommended display bandwidth

The current families are:

- `DEMOD_FAMILY_PSK`: BPSK31 and PSK63
- `DEMOD_FAMILY_FSK`: RTTY
- `DEMOD_FAMILY_FT`: FT8 and FT4
- `DEMOD_FAMILY_CW`: CW
- `DEMOD_FAMILY_MFSK`: MFSK16

The UI and waterfall use the same profile table, so adding a mode changes both
decoder selection and display bandwidth through one data source.

## Unified Decode Layer

`main/decoder_layer.c` is the common receive dispatcher. It owns shared state
for:

- symbol buffering
- selected demod profile
- PSK/RTTY backend instance
- FT monitor instance
- CW timing state
- MFSK16 interleaver and Viterbi state

The main dispatch is:

```c
switch (profile->family) {
case DEMOD_FAMILY_PSK:
case DEMOD_FAMILY_FSK:
    psk_rtty_rx_feed(...);
    break;
case DEMOD_FAMILY_FT:
    feed_ft_symbols(...);
    break;
case DEMOD_FAMILY_CW:
    feed_cw(...);
    break;
case DEMOD_FAMILY_MFSK:
    feed_mfsk16(...);
    break;
}
```

This keeps one public decoder layer while allowing each modulation family to
compose only the primitives it needs.

## DSP Primitives

`main/demod_primitives.c` provides small reusable DSP helpers:

- mode profile lookup
- tone clamping
- tone power measurement
- Goertzel-style tone power wrapper

Higher-level decoders should use these helpers where possible instead of
duplicating frequency and tone-energy logic.

## BPSK31, PSK63, and RTTY

The renamed `components/psk_rtty` component handles:

- BPSK31
- PSK63
- RTTY

The component contains the existing PSK/RTTY demodulation and decoding logic,
but all external symbols now use the `psk_rtty` namespace. The unified decoder
layer maps public `modem_mode_t` values into `psk_rtty_mode_t` and receives
decoded characters through a callback.

This component should stay focused on PSK and 2-tone FSK modes. Shared
selection, output buffering, display behavior, and transport are outside this
component.

## FT8 and FT4

FT8 and FT4 use the vendored `components/ft8lib` backend based on the portable
`kgoba/ft8_lib` C implementation.

The receive path is:

```text
audio samples
    -> monitor_process()
    -> waterfall block accumulation
    -> ftx_find_candidates()
    -> ftx_decode_candidate()
    -> LDPC and CRC validation
    -> ftx_message_decode()
    -> decoded text line
```

The decoder layer creates `monitor_t` lazily when FT8 or FT4 is selected. The
monitor protocol is selected from the active modem mode:

- `MODEM_MODE_FT8` -> `FTX_PROTOCOL_FT8`
- `MODEM_MODE_FT4` -> `FTX_PROTOCOL_FT4`

Callsign hash lookup/save is provided by a small in-memory hash table in the
decoder layer.

## CW

CW is implemented in the unified layer as a lightweight tone-energy and timing
decoder.

The receive path is:

```text
audio samples
    -> tone power at selected CW tone
    -> side-tone/noise comparison
    -> mark/space run lengths
    -> adaptive dit unit estimate
    -> Morse tree table
    -> ASCII characters
```

The unit estimate starts at 20 WPM and adapts between bounded 8 WPM and 45 WPM
limits. This keeps the decoder usable for manual CW without adding a large
desktop-style state machine.

## MFSK16

MFSK16 is implemented as a composed protocol chain inside the unified layer.

The receive path is:

```text
one MFSK symbol of audio
    -> 16 tone energy measurements
    -> Gray reverse mapping
    -> 4 soft bits
    -> depth-10 square deinterleaver
    -> NASA K=7 convolutional Viterbi decoder
    -> MFSK varicode delimiter detection
    -> ASCII characters
```

This replaces the earlier raw-tone/nibble output path. MFSK16 now emits decoded
characters instead of raw tone indexes.

The implementation intentionally keeps the pieces small:

- tone bank measurement
- soft-bit generation
- deinterleaver
- Viterbi traceback
- varicode decode

These can be improved independently without changing the modem API.

## UI and Settings

The user button toggles the settings screen:

- press once: enter settings
- press again: leave settings

The settings UI supports:

- modulation mode selection by touch
- output transport selection by touch

Only one output transport is active at a time:

- USB serial
- WiFi
- BT

The UI sends mode, tone, and transport events through the control queue. The
audio task applies these events to the modem and transport layers.

## Transport

`main/transport.c` owns decoded byte delivery.

The transport mode is exclusive. Switching mode disables the old path and uses
the newly selected path for subsequent decoded bytes.

Current transport behavior:

- USB serial writes decoded bytes to the USB serial output path.
- WiFi sends decoded bytes over the configured WiFi path.
- BT is represented as a selectable mode and should be completed with the
  target BLE/SPP implementation required by the product.

Transport receives decoded bytes as raw binary data and must not assume
NUL-terminated strings.

## Spectrum and Waterfall

The spectrum analyzer and display run beside the decoder. Display bandwidth is
derived from the active `demod_profile_t`.

Touch interaction maps screen position to a new center tone inside the selected
mode bandwidth. The selected tone is then applied to the modem through
`modem_set_rx_tone()`.

This means every mode has one source of truth for:

- decoder parameters
- waterfall display width
- valid touch tuning range

## Extension Rules

When adding or improving a mode:

1. Add or update its `demod_profile_t`.
2. Reuse existing DSP primitives where possible.
3. Add only the smallest backend state needed by the modulation family.
4. Keep public mode switching in `modem.c` and dispatch in `decoder_layer.c`.
5. Emit decoded bytes through the unified callback.
6. Do not add another top-level decoder API for a single mode.

When adding a transport:

1. Keep it behind `transport_send()`.
2. Preserve exclusive mode selection.
3. Treat output as binary bytes, not C strings.

## Licensing Notes

`components/ft8lib` is vendored from an MCU-oriented FT8/FT4 C implementation.
The PSK/RTTY code is retained in project form under the renamed `psk_rtty`
component.

Fldigi and uSDX are useful algorithm references, but the project should avoid
bulk-copying desktop GPL implementation files unless the whole repository is
intentionally managed under compatible license obligations.
