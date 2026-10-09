#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>

#include "order_book.h"

namespace {
    std::atomic<std::size_t> g_allocationCount{0};
    std::atomic<std::size_t> g_deallocationCount{0};
    std::atomic<bool> g_trackingEnabled{false};
}

void* operator new(std::size_t size) {
    if (g_trackingEnabled.load(std::memory_order_relaxed)) {
        g_allocationCount.fetch_add(1, std::memory_order_relaxed);
    }
    void* ptr = std::malloc(size);
    if (!ptr) throw std::bad_alloc();
    return ptr;
}

void operator delete(void* ptr) noexcept {
    if (ptr) {
        if (g_trackingEnabled.load(std::memory_order_relaxed)) {
            g_deallocationCount.fetch_add(1, std::memory_order_relaxed);
        }
        std::free(ptr);
    }
}

void operator delete(void* ptr, std::size_t) noexcept {
    ::operator delete(ptr);
}

class AllocationTracker {
public:
    AllocationTracker() {
        g_allocationCount.store(0, std::memory_order_relaxed);
        g_deallocationCount.store(0, std::memory_order_relaxed);
        g_trackingEnabled.store(true, std::memory_order_release);
    }

    ~AllocationTracker() {
        g_trackingEnabled.store(false, std::memory_order_release);
    }

    std::size_t allocations() const {
        return g_allocationCount.load(std::memory_order_acquire);
    }

    std::size_t deallocations() const {
        return g_deallocationCount.load(std::memory_order_acquire);
    }

    void reset() {
        g_allocationCount.store(0, std::memory_order_relaxed);
        g_deallocationCount.store(0, std::memory_order_relaxed);
    }
};

class AllocationTest : public ::testing::Test {
protected:
    std::vector<Trade> trades_;

    void SetUp() override {
        trades_.clear();
        trades_.reserve(1000);  // Pre-allocate to avoid allocations in callback
    }

    auto makeBook(std::size_t capacity = 100) {
        return OrderBook(capacity, [this](const Trade& t) { trades_.push_back(t); });
    }
};

TEST_F(AllocationTest, AddLimitOrderAllocatesOnlyForIndex) {
    auto book = makeBook(100);

    AllocationTracker tracker;

    for (int i = 0; i < 50; ++i) {
        book.addLimitOrder(Side::Buy, 100 - static_cast<uint32_t>(i), 10, static_cast<uint64_t>(i), 1);
    }

    // unordered_map allocates one node per entry - this is expected

    EXPECT_EQ(tracker.allocations(), 50)
        << "Only orderIndex_ node allocation expected (1 per order)";
}

TEST_F(AllocationTest, MatchingNoAllocation) {
    auto book = makeBook(100);

    for (int i = 0; i < 20; ++i) {
        book.addLimitOrder(Side::Sell, 100 + static_cast<uint32_t>(i), 10, static_cast<uint64_t>(i), 1);
    }

    AllocationTracker tracker;

    for (int i = 0; i < 20; ++i) {
        book.addLimitOrder(Side::Buy, 100 + static_cast<uint32_t>(i), 10, static_cast<uint64_t>(100 + i), 2);
    }

    EXPECT_EQ(tracker.allocations(), 0)
        << "Matching should not allocate";
}

TEST_F(AllocationTest, CancelOrderNoAllocation) {
    auto book = makeBook(100);

    for (int i = 0; i < 20; ++i) {
        book.addLimitOrder(Side::Buy, 100, 10, static_cast<uint64_t>(i), 1);
    }

    AllocationTracker tracker;

    for (int i = 0; i < 20; ++i) {
        book.cancelOrder(static_cast<uint64_t>(i));
    }

    EXPECT_EQ(tracker.allocations(), 0)
        << "cancelOrder should not allocate";
}

TEST_F(AllocationTest, MixedOperationsOnlyIndexAllocates) {
    auto book = makeBook(200);

    for (int i = 0; i < 50; ++i) {
        book.addLimitOrder(Side::Buy, 90 + static_cast<uint32_t>(i % 10), 10, static_cast<uint64_t>(i), 1);
        book.addLimitOrder(Side::Sell, 110 + static_cast<uint32_t>(i % 10), 10, static_cast<uint64_t>(1000 + i), 2);
    }

    AllocationTracker tracker;

    for (int i = 0; i < 30; ++i) {

        book.addLimitOrder(Side::Buy, 80, 5, static_cast<uint64_t>(2000 + i), 3);

        book.addLimitOrder(Side::Buy, 120, 5, static_cast<uint64_t>(3000 + i), 4);

        book.cancelOrder(static_cast<uint64_t>(2000 + i));
    }

    EXPECT_EQ(tracker.allocations(), 30)
        << "Only orderIndex_ insertions should allocate";
}

TEST_F(AllocationTest, FullCycleOnlyIndexAllocates) {
    auto book = makeBook(100);

    AllocationTracker tracker;

    for (int round = 0; round < 5; ++round) {

        for (int i = 0; i < 10; ++i) {
            book.addLimitOrder(Side::Sell, 100, 10,
                static_cast<uint64_t>(round * 100 + i), 1);
        }

        for (int i = 0; i < 10; ++i) {
            book.addLimitOrder(Side::Buy, 100, 10,
                static_cast<uint64_t>(round * 100 + 50 + i), 2);
        }
    }

    EXPECT_EQ(tracker.allocations(), 50)
        << "Only orderIndex_ insertions should allocate";
}
