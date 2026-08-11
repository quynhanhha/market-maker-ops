#include <gtest/gtest.h>
#include <vector>

#include "order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// TEST FIXTURE
// ─────────────────────────────────────────────────────────────────────────────

class OrderBookCancelTest : public ::testing::Test {
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
// 1. CANCEL NON-EXISTENT ORDER
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookCancelTest, CancelNonExistentOrderIsNoOp) {
    auto book = makeBook();

    // Should not crash or throw
    book.cancelOrder(999);

    EXPECT_TRUE(trades_.empty());
    EXPECT_EQ(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(OrderBookCancelTest, CancelAlreadyCancelledOrderIsNoOp) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);
    book.cancelOrder(1);
    
    // Cancel again - should be no-op
    book.cancelOrder(1);

    EXPECT_TRUE(trades_.empty());
    EXPECT_EQ(book.bestBid(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. CANCEL HEAD OF QUEUE
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookCancelTest, CancelHeadBidLeavesRemainingOrders) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 10, 1, 100);  // head
    book.addLimitOrder(Side::Buy, 100, 20, 2, 101);  // middle
    book.addLimitOrder(Side::Buy, 100, 30, 3, 102);  // tail

    book.cancelOrder(1);  // cancel head

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 50);  // 20 + 30
}

TEST_F(OrderBookCancelTest, CancelHeadAskLeavesRemainingOrders) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 10, 1, 100);  // head
    book.addLimitOrder(Side::Sell, 100, 20, 2, 101);  // middle
    book.addLimitOrder(Side::Sell, 100, 30, 3, 102);  // tail

    book.cancelOrder(1);  // cancel head

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 50);  // 20 + 30
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. CANCEL MIDDLE OF QUEUE
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookCancelTest, CancelMiddleBidLeavesHeadAndTail) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 10, 1, 100);  // head
    book.addLimitOrder(Side::Buy, 100, 20, 2, 101);  // middle
    book.addLimitOrder(Side::Buy, 100, 30, 3, 102);  // tail

    book.cancelOrder(2);  // cancel middle

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 40);  // 10 + 30
}

TEST_F(OrderBookCancelTest, CancelMiddleAskLeavesHeadAndTail) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 10, 1, 100);  // head
    book.addLimitOrder(Side::Sell, 100, 20, 2, 101);  // middle
    book.addLimitOrder(Side::Sell, 100, 30, 3, 102);  // tail

    book.cancelOrder(2);  // cancel middle

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 40);  // 10 + 30
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. CANCEL TAIL OF QUEUE
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookCancelTest, CancelTailBidLeavesHeadAndMiddle) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 10, 1, 100);  // head
    book.addLimitOrder(Side::Buy, 100, 20, 2, 101);  // middle
    book.addLimitOrder(Side::Buy, 100, 30, 3, 102);  // tail

    book.cancelOrder(3);  // cancel tail

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 30);  // 10 + 20
}

TEST_F(OrderBookCancelTest, CancelTailAskLeavesHeadAndMiddle) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 10, 1, 100);  // head
    book.addLimitOrder(Side::Sell, 100, 20, 2, 101);  // middle
    book.addLimitOrder(Side::Sell, 100, 30, 3, 102);  // tail

    book.cancelOrder(3);  // cancel tail

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 30);  // 10 + 20
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. CANCEL ONLY ORDER → PRICE LEVEL REMOVED
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookCancelTest, CancelOnlyBidRemovesPriceLevel) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);

    book.cancelOrder(1);

    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(OrderBookCancelTest, CancelOnlyAskRemovesPriceLevel) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);

    book.cancelOrder(1);

    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. BEST BID/ASK UPDATES CORRECTLY
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookCancelTest, CancelBestBidUpdatesToNextLevel) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 102, 10, 1, 100);  // best bid
    book.addLimitOrder(Side::Buy, 101, 20, 2, 101);  // second best
    book.addLimitOrder(Side::Buy, 100, 30, 3, 102);  // worst

    EXPECT_EQ(book.bestBid()->price, 102);

    book.cancelOrder(1);  // cancel best bid

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 101);  // now best
}

TEST_F(OrderBookCancelTest, CancelBestAskUpdatesToNextLevel) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 10, 1, 100);  // best ask
    book.addLimitOrder(Side::Sell, 101, 20, 2, 101);  // second best
    book.addLimitOrder(Side::Sell, 102, 30, 3, 102);  // worst

    EXPECT_EQ(book.bestAsk()->price, 100);

    book.cancelOrder(1);  // cancel best ask

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 101);  // now best
}

TEST_F(OrderBookCancelTest, CancelNonBestLevelDoesNotAffectBest) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 102, 10, 1, 100);  // best bid
    book.addLimitOrder(Side::Buy, 100, 20, 2, 101);  // worse bid

    book.cancelOrder(2);  // cancel worse level

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 102);  // unchanged
    EXPECT_EQ(book.bestBid()->totalQuantity, 10);
}
