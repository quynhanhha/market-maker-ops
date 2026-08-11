#include <gtest/gtest.h>

#include "price_level.h"
#include "order_pool.h"

// ─────────────────────────────────────────────────────────────────────────────
// CONSTRUCTION / EMPTY STATE
// ─────────────────────────────────────────────────────────────────────────────

TEST(PriceLevelTest, StartsEmpty) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};

    EXPECT_TRUE(pl.isEmpty());
    EXPECT_EQ(pl.totalQuantity, 0);
    EXPECT_EQ(pl.head, nullptr);
    EXPECT_EQ(pl.tail, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// ADD ORDERS
// ─────────────────────────────────────────────────────────────────────────────

TEST(PriceLevelTest, AddSingleOrder) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(1);

    Order* o = pool.allocate();
    o->orderId = 1;
    o->quantity = 50;
    o->price = 100;

    pl.addToTail(o);

    EXPECT_EQ(pl.head, o);
    EXPECT_EQ(pl.tail, o);
    EXPECT_EQ(pl.totalQuantity, 50);
    EXPECT_EQ(o->next, nullptr);
    EXPECT_EQ(o->prev, nullptr);
    EXPECT_FALSE(pl.isEmpty());
}

TEST(PriceLevelTest, FIFOOrdering) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();

    o1->orderId = 1; o1->quantity = 10;
    o2->orderId = 2; o2->quantity = 20;
    o3->orderId = 3; o3->quantity = 30;

    pl.addToTail(o1);
    pl.addToTail(o2);
    pl.addToTail(o3);

    // Traverse from head: o1 -> o2 -> o3
    EXPECT_EQ(pl.head, o1);
    EXPECT_EQ(pl.head->next, o2);
    EXPECT_EQ(pl.head->next->next, o3);
    EXPECT_EQ(pl.tail, o3);

    // Backward links
    EXPECT_EQ(o3->prev, o2);
    EXPECT_EQ(o2->prev, o1);
    EXPECT_EQ(o1->prev, nullptr);

    EXPECT_EQ(pl.totalQuantity, 60);
}

// ─────────────────────────────────────────────────────────────────────────────
// REMOVE ORDERS
// ─────────────────────────────────────────────────────────────────────────────

TEST(PriceLevelTest, RemoveHead) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();

    o1->orderId = 1; o1->quantity = 10;
    o2->orderId = 2; o2->quantity = 20;
    o3->orderId = 3; o3->quantity = 30;

    pl.addToTail(o1);
    pl.addToTail(o2);
    pl.addToTail(o3);

    pl.remove(o1);

    EXPECT_EQ(pl.head, o2);
    EXPECT_EQ(pl.tail, o3);
    EXPECT_EQ(o2->prev, nullptr);
    EXPECT_EQ(o2->next, o3);
    EXPECT_EQ(pl.totalQuantity, 50);  // 20 + 30
}

TEST(PriceLevelTest, RemoveTail) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();

    o1->orderId = 1; o1->quantity = 10;
    o2->orderId = 2; o2->quantity = 20;
    o3->orderId = 3; o3->quantity = 30;

    pl.addToTail(o1);
    pl.addToTail(o2);
    pl.addToTail(o3);

    pl.remove(o3);

    EXPECT_EQ(pl.head, o1);
    EXPECT_EQ(pl.tail, o2);
    EXPECT_EQ(o2->next, nullptr);
    EXPECT_EQ(o1->next, o2);
    EXPECT_EQ(pl.totalQuantity, 30);  
}

TEST(PriceLevelTest, RemoveMiddle) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(3);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    Order* o3 = pool.allocate();

    o1->orderId = 1; o1->quantity = 10;
    o2->orderId = 2; o2->quantity = 20;
    o3->orderId = 3; o3->quantity = 30;

    pl.addToTail(o1);
    pl.addToTail(o2);
    pl.addToTail(o3);

    pl.remove(o2);

    EXPECT_EQ(pl.head, o1);
    EXPECT_EQ(pl.tail, o3);
    EXPECT_EQ(o1->next, o3);
    EXPECT_EQ(o3->prev, o1);
    EXPECT_EQ(pl.totalQuantity, 40);  
}

TEST(PriceLevelTest, RemoveOnlyOrderMakesLevelEmpty) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(1);

    Order* o = pool.allocate();
    o->orderId = 1;
    o->quantity = 50;

    pl.addToTail(o);
    EXPECT_FALSE(pl.isEmpty());

    pl.remove(o);

    EXPECT_TRUE(pl.isEmpty());
    EXPECT_EQ(pl.totalQuantity, 0);
    EXPECT_EQ(pl.head, nullptr);
    EXPECT_EQ(pl.tail, nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// FRONT
// ─────────────────────────────────────────────────────────────────────────────

TEST(PriceLevelTest, FrontReturnsHead) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};
    OrderPool pool(2);

    Order* o1 = pool.allocate();
    Order* o2 = pool.allocate();
    o1->orderId = 1; o1->quantity = 10;
    o2->orderId = 2; o2->quantity = 20;

    pl.addToTail(o1);
    EXPECT_EQ(pl.front(), o1);

    pl.addToTail(o2);
    EXPECT_EQ(pl.front(), o1);
}

TEST(PriceLevelTest, FrontReturnsNullWhenEmpty) {
    PriceLevel pl{.price = 100, .totalQuantity = 0, .head = nullptr, .tail = nullptr};

    EXPECT_EQ(pl.front(), nullptr);
}

