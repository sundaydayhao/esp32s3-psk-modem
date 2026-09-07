#include "modem.h"
#include <assert.h>
#include <stdio.h>

int main(void)
{
    static modem_t modem;
    const modem_config_t config = {.mode = MODEM_MODE_PSK63,
        .psk_mode = PSK_MODE_8PSK1200F, .sample_rate_hz = 16000,
        .rx_tone_hz = 1500, .squelch = 30};
    modem_init(&modem, &config);
    assert(modem.decoder.psk_rtty_rx.config.mode == PSK_RTTY_MODE_8PSK1200F);
    /* Same operation used to recover a missing DMA block. */
    modem_set_mode(&modem, modem_get_mode(&modem));
    assert(modem_get_psk_mode(&modem) == PSK_MODE_8PSK1200F);
    assert(modem.decoder.psk_rtty_rx.config.mode == PSK_RTTY_MODE_8PSK1200F);
    modem_set_psk_mode(&modem, PSK_MODE_BPSK500);
    modem_set_mode(&modem, modem_get_mode(&modem));
    assert(modem.decoder.psk_rtty_rx.config.mode == PSK_RTTY_MODE_BPSK500);
    modem_set_psk_mode(&modem, PSK_MODE_8PSK125F);
    assert(modem_get_psk_mode(&modem) == PSK_MODE_BPSK500); /* Disabled K=16 mode. */
    assert(!psk_mode_can_decode((psk_mode_t)-1));
    puts("modem reset: selected backend preserved; unavailable modes rejected PASS");
}
