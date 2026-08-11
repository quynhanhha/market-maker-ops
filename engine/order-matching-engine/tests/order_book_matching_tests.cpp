#include <gtest/gtest.h>
#include <vector>

#include "order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// TEST FIXTURE
// ─────────────────────────────────────────────────────────────────────────────

class OrderBookMatchingTest : public ::testing::Test {
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
// 1. NO MATCHING (orders rest on book)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuyOrderRestsWhenNoAsks) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);

    EXPECT_TRUE(trades_.empty());
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 50);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(OrderBookMatchingTest, SellOrderRestsWhenNoBids) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    EXPECT_TRUE(trades_.empty());
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 50);
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(OrderBookMatchingTest, BuyOrderRestsWhenPriceBelowBestAsk) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);  // ask @ 100
    book.addLimitOrder(Side::Buy, 99, 50, 2, 200);    // buy @ 99, no cross

    EXPECT_TRUE(trades_.empty());
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 99);
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
}

TEST_F(OrderBookMatchingTest, SellOrderRestsWhenPriceAboveBestBid) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);   // bid @ 100
    book.addLimitOrder(Side::Sell, 101, 50, 2, 200);  // sell @ 101, no cross

    EXPECT_TRUE(trades_.empty());
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 101);
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. EXACT FILL (incoming fully fills, resting fully fills)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuyExactlyFillsSell) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);
    book.addLimitOrder(Side::Buy, 100, 50, 2, 200);

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 2);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 50);

    EXPECT_EQ(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(OrderBookMatchingTest, SellExactlyFillsBuy) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);
    book.addLimitOrder(Side::Sell, 100, 50, 2, 200);

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 2);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 50);

    EXPECT_EQ(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. PARTIAL FILL - INCOMING REMAINDER RESTS
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuyPartiallyFillsRemainderRests) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 30, 1, 100);  // resting 30
    book.addLimitOrder(Side::Buy, 100, 50, 2, 200);   // incoming 50

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 2);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 30);

    EXPECT_EQ(book.bestAsk(), nullptr);  // resting fully filled
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 20);  // 50 - 30 remains
}

TEST_F(OrderBookMatchingTest, SellPartiallyFillsRemainderRests) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 30, 1, 100);   // resting 30
    book.addLimitOrder(Side::Sell, 100, 50, 2, 200);  // incoming 50

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 2);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 30);

    EXPECT_EQ(book.bestBid(), nullptr);  // resting fully filled
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 20);  // 50 - 30 remains
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. PARTIAL FILL - RESTING REMAINDER STAYS
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuyPartiallyFillsRestingRemains) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);  // resting 50
    book.addLimitOrder(Side::Buy, 100, 30, 2, 200);   // incoming 30

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 2);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 30);

    EXPECT_EQ(book.bestBid(), nullptr);  // incoming fully filled
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 20);  // 50 - 30 remains
}

TEST_F(OrderBookMatchingTest, SellPartiallyFillsRestingRemains) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);   // resting 50
    book.addLimitOrder(Side::Sell, 100, 30, 2, 200);  // incoming 30

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 2);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 30);

    EXPECT_EQ(book.bestAsk(), nullptr);  // incoming fully filled
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 20);  // 50 - 30 remains
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. MULTI-ORDER MATCHING (same price level - FIFO)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuySweepsMultipleOrdersSamePriceFIFO) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 20, 1, 100);  // first
    book.addLimitOrder(Side::Sell, 100, 30, 2, 101);  // second
    book.addLimitOrder(Side::Buy, 100, 40, 3, 200);   // sweeps first fully, second partially

    ASSERT_EQ(trades_.size(), 2);

    // First trade: fills order 1 completely
    EXPECT_EQ(trades_[0].buyOrderId, 3);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 20);

    // Second trade: fills order 2 partially
    EXPECT_EQ(trades_[1].buyOrderId, 3);
    EXPECT_EQ(trades_[1].sellOrderId, 2);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 20);

    EXPECT_EQ(book.bestBid(), nullptr);
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 10);  // 30 - 20 remains
}

TEST_F(OrderBookMatchingTest, SellSweepsMultipleOrdersSamePriceFIFO) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 20, 1, 100);   // first
    book.addLimitOrder(Side::Buy, 100, 30, 2, 101);   // second
    book.addLimitOrder(Side::Sell, 100, 40, 3, 200);  // sweeps first fully, second partially

    ASSERT_EQ(trades_.size(), 2);

    // First trade: fills order 1 completely
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 3);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 20);

    // Second trade: fills order 2 partially
    EXPECT_EQ(trades_[1].buyOrderId, 2);
    EXPECT_EQ(trades_[1].sellOrderId, 3);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 20);

    EXPECT_EQ(book.bestAsk(), nullptr);
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 10);  // 30 - 20 remains
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. MULTI-LEVEL MATCHING (price priority)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuySweepsMultiplePriceLevelsBestFirst) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 20, 1, 100);  // best ask
    book.addLimitOrder(Side::Sell, 101, 30, 2, 101);  // worse ask
    book.addLimitOrder(Side::Buy, 101, 40, 3, 200);   // sweeps 100@20, then 101@20

    ASSERT_EQ(trades_.size(), 2);

    // First trade at best price (100)
    EXPECT_EQ(trades_[0].buyOrderId, 3);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 20);

    // Second trade at next price (101)
    EXPECT_EQ(trades_[1].buyOrderId, 3);
    EXPECT_EQ(trades_[1].sellOrderId, 2);
    EXPECT_EQ(trades_[1].price, 101);
    EXPECT_EQ(trades_[1].quantity, 20);

    EXPECT_EQ(book.bestBid(), nullptr);
    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 101);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 10);
}

TEST_F(OrderBookMatchingTest, SellSweepsMultiplePriceLevelsBestFirst) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 101, 20, 1, 100);  // best bid
    book.addLimitOrder(Side::Buy, 100, 30, 2, 101);  // worse bid
    book.addLimitOrder(Side::Sell, 100, 40, 3, 200); // sweeps 101@20, then 100@20

    ASSERT_EQ(trades_.size(), 2);

    // First trade at best price (101)
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 3);
    EXPECT_EQ(trades_[0].price, 101);
    EXPECT_EQ(trades_[0].quantity, 20);

    // Second trade at next price (100)
    EXPECT_EQ(trades_[1].buyOrderId, 2);
    EXPECT_EQ(trades_[1].sellOrderId, 3);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 20);

    EXPECT_EQ(book.bestAsk(), nullptr);
    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 10);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. PRICE IMPROVEMENT (aggressive price crosses spread)
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, BuyWithPriceImprovementMatchesAtAskPrice) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);
    book.addLimitOrder(Side::Buy, 105, 50, 2, 200);  // willing to pay more

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 2);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);  // trades at resting price, not 105
    EXPECT_EQ(trades_[0].quantity, 50);

    EXPECT_EQ(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(OrderBookMatchingTest, SellWithPriceImprovementMatchesAtBidPrice) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);
    book.addLimitOrder(Side::Sell, 95, 50, 2, 200);  // willing to accept less

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 2);
    EXPECT_EQ(trades_[0].price, 100);  // trades at resting price, not 95
    EXPECT_EQ(trades_[0].quantity, 50);

    EXPECT_EQ(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. BOOK INTEGRITY AFTER OPERATIONS
// ─────────────────────────────────────────────────────────────────────────────

TEST_F(OrderBookMatchingTest, PriceLevelRemovedWhenAllOrdersFilled) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 20, 1, 100);
    book.addLimitOrder(Side::Sell, 100, 30, 2, 101);
    book.addLimitOrder(Side::Buy, 100, 50, 3, 200);  // fills both completely

    ASSERT_EQ(trades_.size(), 2);

    // Verify both trades
    EXPECT_EQ(trades_[0].buyOrderId, 3);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 20);

    EXPECT_EQ(trades_[1].buyOrderId, 3);
    EXPECT_EQ(trades_[1].sellOrderId, 2);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 30);

    // Price level 100 should be completely removed
    EXPECT_EQ(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(OrderBookMatchingTest, MultiplePriceLevelsOrdered) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 10, 1, 100);
    book.addLimitOrder(Side::Buy, 102, 10, 2, 101);  // best bid
    book.addLimitOrder(Side::Buy, 101, 10, 3, 102);

    book.addLimitOrder(Side::Sell, 105, 10, 4, 200);
    book.addLimitOrder(Side::Sell, 103, 10, 5, 201);  // best ask
    book.addLimitOrder(Side::Sell, 104, 10, 6, 202);

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 102);

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 103);
}
