#include <gtest/gtest.h>
#include <vector>

#include "order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// TEST FIXTURE
// ─────────────────────────────────────────────────────────────────────────────

class SelfMatchPreventionTest : public ::testing::Test {
protected:
    std::vector<Trade> trades_;

    void SetUp() override {
        trades_.clear();
    }

    auto makeBook(std::size_t capacity = 10) {
        return OrderBook(capacity, [this](const Trade& t) { trades_.push_back(t); });
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// 1. BASIC SMP - INCOMING ORDER CANCELLED
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, BuyCancelsIncoming) {
    auto book = makeBook();

    // Participant 100 places a sell order
    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    // Same participant 100 tries to buy - should be cancelled (no trade)
    book.addLimitOrder(Side::Buy, 100, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());  // no trade executed

    // Resting sell order should remain untouched
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 50);

    // No buy order should be resting
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, SellCancelsIncoming) {
    auto book = makeBook();

    // Participant 100 places a buy order
    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);

    // Same participant 100 tries to sell - should be cancelled (no trade)
    book.addLimitOrder(Side::Sell, 100, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());  // no trade executed

    // Resting buy order should remain untouched
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 50);

    // No sell order should be resting
    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. DIFFERENT PARTICIPANTS CAN STILL TRADE
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, DifferentParticipantsCanTrade) {
    auto book = makeBook();

    // Participant 100 places a sell order
    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    // Different participant 200 buys - should trade normally
    book.addLimitOrder(Side::Buy, 100, 50, 2, 200);

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 2);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 50);

    EXPECT_EQ(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. SMP WHEN OWN ORDER IS AT FRONT OF QUEUE
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, CancelsIncomingWhenOwnOrderAtFront) {
    auto book = makeBook();

    // Participant 100's order is FIRST in the queue (will be matched first due to FIFO)
    book.addLimitOrder(Side::Sell, 100, 30, 1, 100);  // participant 100 - first
    book.addLimitOrder(Side::Sell, 100, 30, 2, 200);  // participant 200 - second

    // Participant 100 tries to buy - hits own order first → SMP cancels incoming
    book.addLimitOrder(Side::Buy, 100, 50, 3, 100);

    // No trades - SMP triggered on first order in queue
    EXPECT_TRUE(trades_.empty());

    // Both resting orders should remain untouched
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 60);  // 30 + 30

    // Incoming buy was cancelled, not rested
    EXPECT_EQ(book.bestBid(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. SMP WITH AGGRESSIVE PRICE CROSSING
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, BuyAggressivePriceCrossing) {
    auto book = makeBook();

    // Participant 100 has a sell order
    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    // Same participant places aggressive buy (price > ask)
    book.addLimitOrder(Side::Buy, 110, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());  // self-match prevented

    // Sell order remains, buy is cancelled
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 50);
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, SellAggressivePriceCrossing) {
    auto book = makeBook();

    // Participant 100 has a buy order
    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);

    // Same participant places aggressive sell (price < bid)
    book.addLimitOrder(Side::Sell, 90, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());  // self-match prevented

    // Buy order remains, sell is cancelled
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->totalQuantity, 50);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. PARTIAL FILL THEN SMP (CROSS-LEVEL)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, PartialFillThenSelfMatchCrossLevel) {
    auto book = makeBook();

    // Participant 200 has a sell order at 100
    book.addLimitOrder(Side::Sell, 100, 20, 1, 200);
    // Participant 100 has a sell order at 101
    book.addLimitOrder(Side::Sell, 101, 30, 2, 100);

    // Participant 100 tries to buy 40 @ 101
    // Should fill 20 with participant 200, then cancel when hitting own order at 101
    book.addLimitOrder(Side::Buy, 101, 40, 3, 100);

    // Only one trade occurred (with participant 200)
    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 3);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 20);

    // After partial fill, remaining buy (20 qty) is cancelled due to self-match at 101
    // Sell order at 101 should remain intact
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 101);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 30);

    // No buy order resting (cancelled, not rested)
    EXPECT_EQ(book.bestBid(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. SMP ON MULTI-LEVEL BOOK
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, MultiLevelBookBuySide) {
    auto book = makeBook();

    // Participant 10 has asks at two price levels
    book.addLimitOrder(Side::Sell, 100, 5, 1, 10);  // best ask
    book.addLimitOrder(Side::Sell, 101, 5, 2, 10);  // worse ask

    // Participant 10 tries to buy @ 101 (would cross both levels)
    // SMP triggers immediately on first level (price 100)
    book.addLimitOrder(Side::Buy, 101, 10, 3, 10);

    // No trades - SMP prevented everything
    EXPECT_TRUE(trades_.empty());

    // Best ask (price 100) should be untouched
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 5);

    // No buy order resting
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, MultiLevelBookSellSide) {
    auto book = makeBook();

    // Participant 10 has bids at two price levels
    book.addLimitOrder(Side::Buy, 101, 5, 1, 10);  // best bid
    book.addLimitOrder(Side::Buy, 100, 5, 2, 10);  // worse bid

    // Participant 10 tries to sell @ 100 (would cross both levels)
    // SMP triggers immediately on first level (price 101)
    book.addLimitOrder(Side::Sell, 100, 10, 3, 10);

    // No trades - SMP prevented everything
    EXPECT_TRUE(trades_.empty());

    // Best bid (price 101) should be untouched
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 101);
    EXPECT_EQ(book.bestBid()->totalQuantity, 5);

    // No sell order resting
    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. SMP TRIGGERED MID-LOOP (SAME PRICE LEVEL)
// Proves SMP is evaluated order-by-order, not "all or nothing"
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(SelfMatchPreventionTest, MidLoopBuySide) {
    auto book = makeBook(20);

    // Three sell orders at same price level from different participants
    book.addLimitOrder(Side::Sell, 100, 5, 1, 77);  // o1: participant 77
    book.addLimitOrder(Side::Sell, 100, 5, 2, 77);  // o2: participant 77
    book.addLimitOrder(Side::Sell, 100, 5, 3, 99);  // o3: participant 99

    // Participant 99 tries to buy 20 @ 100
    // Should: match o1 (5), match o2 (5), hit o3 → SMP → cancel remaining
    book.addLimitOrder(Side::Buy, 100, 20, 4, 99);

    // Two trades occurred (o1 and o2)
    ASSERT_EQ(trades_.size(), 2);

    // First trade: o1 fully filled
    EXPECT_EQ(trades_[0].buyOrderId, 4);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 5);

    // Second trade: o2 fully filled
    EXPECT_EQ(trades_[1].buyOrderId, 4);
    EXPECT_EQ(trades_[1].sellOrderId, 2);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 5);

    // o3 remains in book (SMP prevented match)
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 5);  // only o3 remains

    // Remaining 10 qty was cancelled (not rested)
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, MidLoopSellSide) {
    auto book = makeBook(20);

    // Three buy orders at same price level from different participants
    book.addLimitOrder(Side::Buy, 100, 5, 1, 77);  // o1: participant 77
    book.addLimitOrder(Side::Buy, 100, 5, 2, 77);  // o2: participant 77
    book.addLimitOrder(Side::Buy, 100, 5, 3, 99);  // o3: participant 99

    // Participant 99 tries to sell 20 @ 100
    // Should: match o1 (5), match o2 (5), hit o3 → SMP → cancel remaining
    book.addLimitOrder(Side::Sell, 100, 20, 4, 99);

    // Two trades occurred (o1 and o2)
    ASSERT_EQ(trades_.size(), 2);

    // First trade: o1 fully filled
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 4);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 5);

    // Second trade: o2 fully filled
    EXPECT_EQ(trades_[1].buyOrderId, 2);
    EXPECT_EQ(trades_[1].sellOrderId, 4);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 5);

    // o3 remains in book (SMP prevented match)
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 5);  // only o3 remains

    // Remaining 10 qty was cancelled (not rested)
    EXPECT_EQ(book.bestAsk(), nullptr);
}
