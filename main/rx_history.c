#include "rx_history.h"

#include <string.h>

void rx_history_append(rx_history_t *h, const uint8_t *data, size_t len)
{
    if (!data || !len) return;
    size_t kept = len > RX_HISTORY_CAPACITY ? RX_HISTORY_CAPACITY : len;
    uint64_t first = h->end + len - kept;
    data += len - kept;
    size_t pos = first % RX_HISTORY_CAPACITY;
    size_t part = RX_HISTORY_CAPACITY - pos;
    if (part > kept) part = kept;
    memcpy(h->bytes + pos, data, part);
    memcpy(h->bytes, data + part, kept - part);
    h->end += len;
}

size_t rx_history_read(const rx_history_t *h, uint64_t requested,
                      uint8_t *out, size_t capacity, uint64_t *start, uint64_t *lost)
{
    uint64_t oldest = h->end > RX_HISTORY_CAPACITY ? h->end - RX_HISTORY_CAPACITY : 0;
    *lost = requested < oldest ? oldest - requested : 0;
    if (requested < oldest || requested > h->end) requested = oldest;
    *start = requested;
    size_t count = h->end - requested;
    if (count > capacity) count = capacity;
    size_t pos = requested % RX_HISTORY_CAPACITY;
    size_t part = RX_HISTORY_CAPACITY - pos;
    if (part > count) part = count;
    memcpy(out, h->bytes + pos, part);
    memcpy(out + part, h->bytes, count - part);
    return count;
}
