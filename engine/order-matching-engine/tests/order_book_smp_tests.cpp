#include <gtest/gtest.h>
#include <vector>

#include "order_book.h"

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

TEST_F(SelfMatchPreventionTest, BuyCancelsIncoming) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    book.addLimitOrder(Side::Buy, 100, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 50);

    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, SellCancelsIncoming) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);

    book.addLimitOrder(Side::Sell, 100, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 50);

    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(SelfMatchPreventionTest, DifferentParticipantsCanTrade) {
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

TEST_F(SelfMatchPreventionTest, CancelsIncomingWhenOwnOrderAtFront) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 30, 1, 100);
    book.addLimitOrder(Side::Sell, 100, 30, 2, 200);

    book.addLimitOrder(Side::Buy, 100, 50, 3, 100);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 60);

    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, BuyAggressivePriceCrossing) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 50, 1, 100);

    book.addLimitOrder(Side::Buy, 110, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 50);
    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, SellAggressivePriceCrossing) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 100, 50, 1, 100);

    book.addLimitOrder(Side::Sell, 90, 50, 2, 100);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->totalQuantity, 50);
    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(SelfMatchPreventionTest, PartialFillThenSelfMatchCrossLevel) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 20, 1, 200);

    book.addLimitOrder(Side::Sell, 101, 30, 2, 100);

    book.addLimitOrder(Side::Buy, 101, 40, 3, 100);

    ASSERT_EQ(trades_.size(), 1);
    EXPECT_EQ(trades_[0].buyOrderId, 3);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 20);

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 101);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 30);

    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, MultiLevelBookBuySide) {
    auto book = makeBook();

    book.addLimitOrder(Side::Sell, 100, 5, 1, 10);
    book.addLimitOrder(Side::Sell, 101, 5, 2, 10);

    book.addLimitOrder(Side::Buy, 101, 10, 3, 10);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 5);

    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, MultiLevelBookSellSide) {
    auto book = makeBook();

    book.addLimitOrder(Side::Buy, 101, 5, 1, 10);
    book.addLimitOrder(Side::Buy, 100, 5, 2, 10);

    book.addLimitOrder(Side::Sell, 100, 10, 3, 10);

    EXPECT_TRUE(trades_.empty());

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 101);
    EXPECT_EQ(book.bestBid()->totalQuantity, 5);

    EXPECT_EQ(book.bestAsk(), nullptr);
}

TEST_F(SelfMatchPreventionTest, MidLoopBuySide) {
    auto book = makeBook(20);

    book.addLimitOrder(Side::Sell, 100, 5, 1, 77);
    book.addLimitOrder(Side::Sell, 100, 5, 2, 77);
    book.addLimitOrder(Side::Sell, 100, 5, 3, 99);

    book.addLimitOrder(Side::Buy, 100, 20, 4, 99);

    ASSERT_EQ(trades_.size(), 2);

    EXPECT_EQ(trades_[0].buyOrderId, 4);
    EXPECT_EQ(trades_[0].sellOrderId, 1);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 5);

    EXPECT_EQ(trades_[1].buyOrderId, 4);
    EXPECT_EQ(trades_[1].sellOrderId, 2);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 5);

    ASSERT_NE(book.bestAsk(), nullptr);
    EXPECT_EQ(book.bestAsk()->price, 100);
    EXPECT_EQ(book.bestAsk()->totalQuantity, 5);

    EXPECT_EQ(book.bestBid(), nullptr);
}

TEST_F(SelfMatchPreventionTest, MidLoopSellSide) {
    auto book = makeBook(20);

    book.addLimitOrder(Side::Buy, 100, 5, 1, 77);
    book.addLimitOrder(Side::Buy, 100, 5, 2, 77);
    book.addLimitOrder(Side::Buy, 100, 5, 3, 99);

    book.addLimitOrder(Side::Sell, 100, 20, 4, 99);

    ASSERT_EQ(trades_.size(), 2);

    EXPECT_EQ(trades_[0].buyOrderId, 1);
    EXPECT_EQ(trades_[0].sellOrderId, 4);
    EXPECT_EQ(trades_[0].price, 100);
    EXPECT_EQ(trades_[0].quantity, 5);

    EXPECT_EQ(trades_[1].buyOrderId, 2);
    EXPECT_EQ(trades_[1].sellOrderId, 4);
    EXPECT_EQ(trades_[1].price, 100);
    EXPECT_EQ(trades_[1].quantity, 5);

    ASSERT_NE(book.bestBid(), nullptr);
    EXPECT_EQ(book.bestBid()->price, 100);
    EXPECT_EQ(book.bestBid()->totalQuantity, 5);

    EXPECT_EQ(book.bestAsk(), nullptr);
}
