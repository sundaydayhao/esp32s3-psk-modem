#include "rx_history.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    static rx_history_t h;
    static uint8_t source[RX_HISTORY_CAPACITY * 3 + 127], got[RX_HISTORY_CAPACITY];
    for (size_t i = 0; i < sizeof(source); ++i) source[i] = (uint8_t)(i * 13 + i / 257);
    uint64_t start, lost;
    assert(rx_history_read(&h, 0, got, sizeof(got), &start, &lost) == 0);
    rx_history_append(&h, source, 123);
    assert(rx_history_read(&h, 0, got, sizeof(got), &start, &lost) == 123);
    assert(start == 0 && lost == 0 && !memcmp(got, source, 123));
    /* Two phones read independently, including bytes 00 and ff. */
    assert(rx_history_read(&h, 31, got, 40, &start, &lost) == 40);
    assert(start == 31 && !memcmp(got, source + 31, 40));
    rx_history_append(&h, source + 123, sizeof(source) - 123);
    size_t oldest = sizeof(source) - RX_HISTORY_CAPACITY;
    assert(rx_history_read(&h, 123, got, sizeof(got), &start, &lost) == RX_HISTORY_CAPACITY);
    assert(start == oldest && lost == oldest - 123);
    assert(!memcmp(got, source + oldest, RX_HISTORY_CAPACITY));
    assert(rx_history_read(&h, h.end, got, sizeof(got), &start, &lost) == 0);
    assert(rx_history_read(&h, h.end + 1, got, 512, &start, &lost) == 512);
    assert(start == oldest && !memcmp(got, source + oldest, 512));
    /* Cross the 32-bit offset boundary without losing circular indexing. */
    h.end = (UINT64_C(1) << 32) - 12;
    rx_history_append(&h, source, 64);
    assert(rx_history_read(&h, (UINT64_C(1) << 32) - 12, got, 64, &start, &lost) == 64);
    assert(!memcmp(got, source, 64));
    puts("rx_history: binary, wrap, slow clients, stale cursor, 64-bit offsets PASS");
}
