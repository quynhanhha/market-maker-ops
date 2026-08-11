#pragma once

#include <cstdint>

enum class Side { Buy, Sell };

struct Trade {
    uint64_t buyOrderId;
    uint64_t sellOrderId;
    uint32_t price;
    uint32_t quantity;
};