#pragma once

#include "types.h"
#include <cstdint>

struct Order {
    uint64_t orderId;
    uint32_t price;
    uint32_t quantity;
    uint64_t sequence;
    uint64_t participantId;
    Side side;

    Order* next;
    Order* prev;

    void init(uint64_t id, uint32_t p, uint32_t qty, uint64_t seq, Side s, uint64_t partiId) {
        orderId = id;
        price = p;
        quantity = qty;
        sequence = seq;
        side = s;
        participantId = partiId;
    }
};