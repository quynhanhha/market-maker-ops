#include "order_pool.h"

#include <cassert>

OrderPool::OrderPool(std::size_t capacity)
    : capacity_(capacity),
    orders_(std::make_unique<Order[]>(capacity)),
    freeList_(nullptr),
    freeCount_(capacity),
    isAllocated_(capacity, 0)
{
    freeList_ = nullptr;
    for (std::size_t i = 0; i < capacity_ - 1; ++i) {
        orders_[i].next = &orders_[i + 1];
    }
    orders_[capacity_ - 1].next = nullptr;
    freeList_ = &orders_[0];
}

Order* OrderPool::allocate() {
    assert(freeList_ != nullptr);
    assert(freeCount_ > 0);
    
    Order* order = freeList_;
    freeList_ = order->next;

    --freeCount_;

    order->next = nullptr;
    order->prev = nullptr;

    std::ptrdiff_t idx = order - &orders_[0];
    assert(idx >= 0);
    assert(static_cast<std::size_t>(idx) < capacity_);
    isAllocated_[static_cast<std::size_t>(idx)] = 1;

    return order;
}

void OrderPool::deallocate(Order* order) {
    assert(order != nullptr);
    assert(freeCount_ < capacity_);

    std::ptrdiff_t idx = order - &orders_[0];
    assert(idx >= 0);
    assert(static_cast<std::size_t>(idx) < capacity_);
    assert(isAllocated_[static_cast<std::size_t>(idx)] == 1);  // catch double-deallocate
    isAllocated_[static_cast<std::size_t>(idx)] = 0;

    order->next = freeList_;
    freeList_ = order;

    ++freeCount_;
}