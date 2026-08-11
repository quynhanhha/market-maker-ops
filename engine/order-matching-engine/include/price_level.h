#pragma once

#include <cstdint>
#include "order.h"

struct PriceLevel {
    uint32_t price;
    uint32_t totalQuantity;

    Order* head;
    Order* tail;

    void addToTail(Order* o);
    void remove(Order* o);
    bool isEmpty() const;
    Order* front() const;
};