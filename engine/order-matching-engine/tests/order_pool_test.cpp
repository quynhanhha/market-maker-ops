#include <gtest/gtest.h>
#include <vector>

#include "order_pool.h"

// ─────────────────────────────────────────────────────────────────────────────
// CONSTRUCTION
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderPoolTest, ConstructorInitializesCorrectly) {
    const std::size_t N = 10;
    OrderPool pool(N);

    EXPECT_EQ(pool.capacity(), N);
    EXPECT_EQ(pool.freeCount(), N);
}

// ─────────────────────────────────────────────────────────────────────────────
// ALLOCATION
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderPoolTest, AllocatesUpToCapacity) {
    const std::size_t N = 4;
    OrderPool pool(N);

    EXPECT_EQ(pool.freeCount(), N);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();
    Order* o4 = pool.allocate();

    EXPECT_NE(o1, nullptr);
    EXPECT_NE(o2, nullptr); 
    EXPECT_NE(o3, nullptr);
    EXPECT_NE(o4, nullptr);

    EXPECT_EQ(pool.freeCount(), 0);
}

TEST(OrderPoolTest, AllocateReturnsUniquePointers) {
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();

    EXPECT_NE(o1, o2);
    EXPECT_NE(o2, o3);
    EXPECT_NE(o1, o3);
}

TEST(OrderPoolTest, AllocatedOrderHasNullPrevNext) {
    OrderPool pool(2);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();

    EXPECT_EQ(o1->next, nullptr);
    EXPECT_EQ(o1->prev, nullptr);
    EXPECT_EQ(o2->next, nullptr);
    EXPECT_EQ(o2->prev, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// DEALLOCATION
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderPoolTest, DeallocateIncreasesFreeCount) {
    OrderPool pool(2);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    EXPECT_EQ(pool.freeCount(), 0);

    pool.deallocate(o1);
    EXPECT_EQ(pool.freeCount(), 1);

    pool.deallocate(o2);
    EXPECT_EQ(pool.freeCount(), 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// REUSE / LIFO BEHAVIOR
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderPoolTest, ReusesDeallocatedOrders) {
    OrderPool pool(1);

    Order* o1 = pool.allocate();

    EXPECT_EQ(pool.freeCount(), 0);

    pool.deallocate(o1);

    EXPECT_EQ(pool.freeCount(), 1);

    Order* o3 = pool.allocate();
    EXPECT_NE(o3, nullptr);
    EXPECT_EQ(pool.freeCount(), 0);
    EXPECT_EQ(o3, o1);

    EXPECT_EQ(o3->next, nullptr);
    EXPECT_EQ(o3->prev, nullptr);
}

TEST(OrderPoolTest, DeallocateAllocateIsLIFO) {
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();

    pool.deallocate(o1);
    pool.deallocate(o2);
    pool.deallocate(o3);

    EXPECT_EQ(pool.allocate(), o3);
    EXPECT_EQ(pool.allocate(), o2);
    EXPECT_EQ(pool.allocate(), o1);
}

// ─────────────────────────────────────────────────────────────────────────────
// FULL CYCLE
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderPoolTest, FullCycle) {
    const std::size_t N = 5;
    OrderPool pool(N);

    std::vector<Order*> orders;
    for (std::size_t i = 0; i < N; ++i) {
        orders.push_back(pool.allocate());
    }
    EXPECT_EQ(pool.freeCount(), 0);

    for (Order* o : orders) {
        pool.deallocate(o);
    }
    EXPECT_EQ(pool.freeCount(), N);

    for (std::size_t i = 0; i < N; ++i) {
        Order* o = pool.allocate();
        EXPECT_NE(o, nullptr);
        EXPECT_EQ(o->next, nullptr);
        EXPECT_EQ(o->prev, nullptr);
    }
    EXPECT_EQ(pool.freeCount(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// DEATH TESTS
// ─────────────────────────────────────────────────────────────────────────────

TEST(OrderPoolDeathTest, AllocateWhenEmptyAsserts) {
    OrderPool pool(1);
    pool.allocate();  

    EXPECT_DEATH(pool.allocate(), "");
}

TEST(OrderPoolDeathTest, DeallocateNullptrAsserts) {
    OrderPool pool(1);

    EXPECT_DEATH(pool.deallocate(nullptr), "");
}

TEST(OrderPoolDeathTest, DeallocateWhenFullAsserts) {
    OrderPool pool(1);

    Order* o = pool.allocate();
    pool.deallocate(o);  

    EXPECT_DEATH(pool.deallocate(o), "");
}

TEST(OrderPoolDeathTest, DoubleDeallocateAsserts) {
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    (void)pool.allocate(); 

    pool.deallocate(o1);

    EXPECT_DEATH(pool.deallocate(o1), "");
}
