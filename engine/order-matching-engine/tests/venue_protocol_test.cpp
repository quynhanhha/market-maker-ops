#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "protocol.h"
#include "venue.h"

using namespace venue;

// ─────────────────────────────────────────────────────────────────────────────
// Protocol encode / decode
// ─────────────────────────────────────────────────────────────────────────────

TEST(Protocol, ParsesValidNewOrder) {
    auto m = parseInbound(R"({"type":"NEW_ORDER","order_id":7,"side":"BUY","price":100,"quantity":5})");
    EXPECT_FALSE(m.parseError);
    EXPECT_EQ(m.type, MsgType::NewOrder);
    EXPECT_TRUE(m.hasOrderId);
    EXPECT_EQ(m.orderId, 7u);
    EXPECT_TRUE(m.hasSide);
    EXPECT_EQ(m.side, OrderSide::Buy);
    EXPECT_EQ(m.price, 100u);
    EXPECT_EQ(m.quantity, 5u);
}

TEST(Protocol, ParsesCancelAndHeartbeat) {
    auto c = parseInbound(R"({"type":"CANCEL","order_id":42})");
    EXPECT_EQ(c.type, MsgType::Cancel);
    EXPECT_EQ(c.orderId, 42u);

    auto h = parseInbound(R"({"type":"HEARTBEAT"})");
    EXPECT_EQ(h.type, MsgType::Heartbeat);
    EXPECT_FALSE(h.parseError);
}

TEST(Protocol, RejectsMalformed) {
    EXPECT_TRUE(parseInbound("not json").parseError);
    EXPECT_TRUE(parseInbound("{\"type\":\"NEW_ORDER\"").parseError);       // unterminated
    EXPECT_TRUE(parseInbound(R"({"order_id":1})").parseError);            // missing type
    EXPECT_TRUE(parseInbound(R"({"type":5})").parseError);                // type not a string
    EXPECT_TRUE(parseInbound(R"({"type":"NEW_ORDER"} trailing)").parseError);
}

TEST(Protocol, UnknownTypeIsNotAParseError) {
    auto m = parseInbound(R"({"type":"WAT"})");
    EXPECT_FALSE(m.parseError);
    EXPECT_EQ(m.type, MsgType::Unknown);
}

TEST(Protocol, FieldWithWrongJsonTypeIsIgnored) {
    // price given as a string → treated as absent, not a crash.
    auto m = parseInbound(R"({"type":"NEW_ORDER","order_id":1,"side":"BUY","price":"x","quantity":5})");
    EXPECT_FALSE(m.hasPrice);
}

TEST(Protocol, EncodersProduceExpectedShape) {
    EXPECT_NE(encodeAck(3).find(R"("type":"ACK")"), std::string::npos);
    EXPECT_NE(encodeAck(3).find(R"("order_id":3)"), std::string::npos);
    EXPECT_EQ(encodeAck(3).back(), '\n');
    EXPECT_NE(encodeReject(3, "NOPE").find(R"("reason":"NOPE")"), std::string::npos);
    EXPECT_NE(encodeFill(3, OrderSide::Sell, 100, 5).find(R"("side":"SELL")"), std::string::npos);
    EXPECT_NE(encodeMarketData(false, 0, 0, true, 101, 4, 9).find(R"("bid_px":null)"), std::string::npos);
    EXPECT_NE(encodeMarketData(false, 0, 0, true, 101, 4, 9).find(R"("ask_px":101)"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Venue behaviour (validation + routing) via a capturing send seam
// ─────────────────────────────────────────────────────────────────────────────

class VenueTest : public ::testing::Test {
protected:
    std::vector<std::pair<Venue::ConnId, std::string>> sent_;

    Venue makeVenue(std::size_t softCap = 1024) {
        return Venue(softCap + 64, softCap,
                     [this](Venue::ConnId id, std::string msg) {
                         sent_.emplace_back(id, std::move(msg));
                     });
    }

    // Count messages to `conn` whose text contains `needle`.
    int count(Venue::ConnId conn, const std::string& needle) const {
        int n = 0;
        for (const auto& [id, msg] : sent_) {
            if (id == conn && msg.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }
};

TEST_F(VenueTest, AcksAValidRestingOrder) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    EXPECT_EQ(count(1, R"("type":"ACK")"), 1);
    EXPECT_EQ(count(1, R"("type":"FILL")"), 0);
}

TEST_F(VenueTest, RejectsBadFields) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":0})");
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":2,"side":"SELL","price":0,"quantity":5})");
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":3,"price":100,"quantity":5})");
    EXPECT_EQ(count(1, "INVALID_QUANTITY"), 1);
    EXPECT_EQ(count(1, "INVALID_PRICE"), 1);
    EXPECT_EQ(count(1, "INVALID_SIDE"), 1);
}

TEST_F(VenueTest, RejectsDuplicateOrderId) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":101,"quantity":10})");
    EXPECT_EQ(count(1, "DUPLICATE_ORDER_ID"), 1);
}

TEST_F(VenueTest, CancelOwnershipAndUnknown) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    v.handleLine(2, R"({"type":"CANCEL","order_id":1})");   // not owner
    v.handleLine(1, R"({"type":"CANCEL","order_id":999})"); // unknown
    v.handleLine(1, R"({"type":"CANCEL","order_id":1})");   // ok
    EXPECT_EQ(count(2, "NOT_OWNER"), 1);
    EXPECT_EQ(count(1, "UNKNOWN_ORDER"), 1);
    EXPECT_EQ(count(1, R"("type":"ACK")"), 2);  // new order + cancel
}

TEST_F(VenueTest, RejectsSelfCross) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":2,"side":"BUY","price":100,"quantity":10})");
    EXPECT_EQ(count(1, "SELF_MATCH"), 1);
}

TEST_F(VenueTest, CrossBetweenTwoParticipantsFillsBoth) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    v.handleLine(2, R"({"type":"NEW_ORDER","order_id":2,"side":"BUY","price":100,"quantity":10})");
    EXPECT_EQ(count(1, R"("type":"FILL")"), 1);
    EXPECT_EQ(count(2, R"("type":"FILL")"), 1);
    EXPECT_EQ(count(2, R"("type":"ACK")"), 1);
}

TEST_F(VenueTest, CapacityGuardRejectsWhenFull) {
    auto v = makeVenue(/*softCap=*/1);
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":2,"side":"SELL","price":101,"quantity":10})");
    EXPECT_EQ(count(1, R"("type":"ACK")"), 1);
    EXPECT_EQ(count(1, "VENUE_FULL"), 1);
}

TEST_F(VenueTest, DropConnectionRemovesRestingOrders) {
    auto v = makeVenue();
    v.handleLine(1, R"({"type":"NEW_ORDER","order_id":1,"side":"SELL","price":100,"quantity":10})");
    v.dropConnection(1);
    // A different participant crossing at that price should now find nothing.
    v.handleLine(2, R"({"type":"NEW_ORDER","order_id":2,"side":"BUY","price":100,"quantity":10})");
    EXPECT_EQ(count(2, R"("type":"FILL")"), 0);
    EXPECT_EQ(count(2, R"("type":"ACK")"), 1);
}
