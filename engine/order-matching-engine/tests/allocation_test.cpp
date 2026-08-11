#include <gtest/gtest.h>
#include <atomic>
#include <cstdlib>

#include "order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// ALLOCATION TRACKING
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// TEST FIXTURE
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
// ALLOCATION TESTS
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(AllocationTest, AddLimitOrderAllocatesOnlyForIndex) {
    auto book = makeBook(100);
    
    // Start tracking after construction
    AllocationTracker tracker;
    
    // Hot path: add orders that don't match (just rest on book)
    for (int i = 0; i < 50; ++i) {
        book.addLimitOrder(Side::Buy, 100 - static_cast<uint32_t>(i), 10, static_cast<uint64_t>(i), 1);
    }
    
    // unordered_map allocates one node per entry - this is expected
    // All other operations (OrderPool, PriceLevel vectors) are allocation-free
    EXPECT_EQ(tracker.allocations(), 50) 
        << "Only orderIndex_ node allocation expected (1 per order)";
}

TEST_F(AllocationTest, MatchingNoAllocation) {
    auto book = makeBook(100);
    
    // Setup: add resting orders
    for (int i = 0; i < 20; ++i) {
        book.addLimitOrder(Side::Sell, 100 + static_cast<uint32_t>(i), 10, static_cast<uint64_t>(i), 1);
    }
    
    // Start tracking after setup
    AllocationTracker tracker;
    
    // Hot path: incoming orders that match
    for (int i = 0; i < 20; ++i) {
        book.addLimitOrder(Side::Buy, 100 + static_cast<uint32_t>(i), 10, static_cast<uint64_t>(100 + i), 2);
    }
    
    EXPECT_EQ(tracker.allocations(), 0) 
        << "Matching should not allocate";
}

TEST_F(AllocationTest, CancelOrderNoAllocation) {
    auto book = makeBook(100);
    
    // Setup: add orders to cancel
    for (int i = 0; i < 20; ++i) {
        book.addLimitOrder(Side::Buy, 100, 10, static_cast<uint64_t>(i), 1);
    }
    
    // Start tracking after setup
    AllocationTracker tracker;
    
    // Hot path: cancel orders
    for (int i = 0; i < 20; ++i) {
        book.cancelOrder(static_cast<uint64_t>(i));
    }
    
    EXPECT_EQ(tracker.allocations(), 0) 
        << "cancelOrder should not allocate";
}

TEST_F(AllocationTest, MixedOperationsOnlyIndexAllocates) {
    auto book = makeBook(200);
    
    // Setup: seed the book
    for (int i = 0; i < 50; ++i) {
        book.addLimitOrder(Side::Buy, 90 + static_cast<uint32_t>(i % 10), 10, static_cast<uint64_t>(i), 1);
        book.addLimitOrder(Side::Sell, 110 + static_cast<uint32_t>(i % 10), 10, static_cast<uint64_t>(1000 + i), 2);
    }
    
    // Start tracking after setup
    AllocationTracker tracker;
    
    // Hot path: mixed operations
    for (int i = 0; i < 30; ++i) {
        // Add resting order (allocates 1 map node)
        book.addLimitOrder(Side::Buy, 80, 5, static_cast<uint64_t>(2000 + i), 3);
        
        // Add matching order (allocates 1 map node, then removes it)
        book.addLimitOrder(Side::Buy, 120, 5, static_cast<uint64_t>(3000 + i), 4);
        
        // Cancel an order (no allocation)
        book.cancelOrder(static_cast<uint64_t>(2000 + i));
    }
    
    // 30 resting orders added to index
    // 30 matching orders are fully filled and never added to index
    EXPECT_EQ(tracker.allocations(), 30) 
        << "Only orderIndex_ insertions should allocate";
}

TEST_F(AllocationTest, FullCycleOnlyIndexAllocates) {
    auto book = makeBook(100);
    
    AllocationTracker tracker;
    
    // Full cycle: add -> match -> empty book
    // All within pre-allocated capacity
    for (int round = 0; round < 5; ++round) {
        // Add sell orders (10 map node allocations)
        for (int i = 0; i < 10; ++i) {
            book.addLimitOrder(Side::Sell, 100, 10, 
                static_cast<uint64_t>(round * 100 + i), 1);
        }
        
        // Match them all with buy orders (10 map node allocations)
        for (int i = 0; i < 10; ++i) {
            book.addLimitOrder(Side::Buy, 100, 10, 
                static_cast<uint64_t>(round * 100 + 50 + i), 2);
        }
    }
    
    // 5 rounds * 10 sells = 50 map node allocations
    // Buy orders fully match and are never added to index
    EXPECT_EQ(tracker.allocations(), 50) 
        << "Only orderIndex_ insertions should allocate";
}
