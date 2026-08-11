#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include "order.h"

class OrderPool {
public:
    explicit OrderPool(std::size_t capacity);

    OrderPool(const OrderPool&) = delete;
    OrderPool& operator=(const OrderPool&) = delete;
    OrderPool(OrderPool&&) noexcept = default;
    OrderPool& operator=(OrderPool&&) noexcept = default;

    Order* allocate();
    void deallocate(Order* order);

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t freeCount() const noexcept { return freeCount_; }    

private:
    std::size_t capacity_;
    std::unique_ptr<Order[]> orders_;
    Order* freeList_;
    std::size_t freeCount_;
    std::vector<uint8_t> isAllocated_;
};