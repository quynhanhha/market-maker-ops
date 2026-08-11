#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "framing.h"
#include "protocol.h"
#include "venue.h"

using namespace venue;

// ─────────────────────────────────────────────────────────────────────────────
// Protocol encode / decode
// ─────────────────────────────────────────────────────────────────────────────

TEST(Protocol, ParsesValidNewOrder) {
    auto m = parseInbound(
        R"({"type":"NEW_ORDER","client_order_id":"abc123","symbol":"SIM1","side":"BUY","price":100.02,"qty":100,"ts":"t"})");
    EXPECT_FALSE(m.parseError);
    EXPECT_EQ(m.type, MsgType::NewOrder);
    EXPECT_TRUE(m.hasClientOrderId);
    EXPECT_EQ(m.clientOrderId, "abc123");
    EXPECT_TRUE(m.hasSymbol);
    EXPECT_EQ(m.symbol, "SIM1");
    EXPECT_TRUE(m.hasSide);
    EXPECT_EQ(m.side, OrderSide::Buy);
    EXPECT_TRUE(m.hasPrice);
    EXPECT_EQ(m.priceTicks, 10002u);  // 100.02 * 100
    EXPECT_TRUE(m.hasQty);
    EXPECT_EQ(m.qty, 100u);
}

TEST(Protocol, DecimalPriceEdgeCases) {
    EXPECT_EQ(parseInbound(R"({"type":"NEW_ORDER","price":100})").priceTicks, 10000u);
    EXPECT_EQ(parseInbound(R"({"type":"NEW_ORDER","price":100.5})").priceTicks, 10050u);
    EXPECT_FALSE(parseInbound(R"({"type":"NEW_ORDER","price":100.025})").hasPrice);  // >2 dp
    EXPECT_FALSE(parseInbound(R"({"type":"NEW_ORDER","price":-5})").hasPrice);
    EXPECT_FALSE(parseInbound(R"({"type":"NEW_ORDER","price":"x"})").hasPrice);
}

TEST(Protocol, ParsesCancelAndHeartbeat) {
    auto c = parseInbound(R"({"type":"CANCEL","client_order_id":"abc123"})");
    EXPECT_EQ(c.type, MsgType::Cancel);
    EXPECT_EQ(c.clientOrderId, "abc123");
    EXPECT_EQ(parseInbound(R"({"type":"HEARTBEAT"})").type, MsgType::Heartbeat);
}

TEST(Protocol, RejectsMalformed) {
    EXPECT_TRUE(parseInbound("not json").parseError);
    EXPECT_TRUE(parseInbound(R"({"type":"NEW_ORDER")").parseError);   // unterminated
    EXPECT_TRUE(parseInbound(R"({"client_order_id":"x"})").parseError);  // no type
    EXPECT_TRUE(parseInbound(R"({"type":5})").parseError);            // type not a string
}

TEST(Protocol, EncodersMatchSpecShape) {
    EXPECT_NE(encodeAck("abc", "e-1", "t").find(R"("exchange_order_id":"e-1")"), std::string::npos);
    EXPECT_NE(encodeReject("abc", "bad_qty", "t").find(R"("reason":"bad_qty")"), std::string::npos);
    EXPECT_NE(encodeCancelAck("abc", "e-1", "t").find(R"("type":"CANCEL_ACK")"), std::string::npos);
    EXPECT_NE(encodeCancelReject("abc", "already_filled", "t").find(R"("reason":"already_filled")"),
              std::string::npos);
    const std::string fill = encodeFill("e-1", "abc", 40, 10002, 60, "t");
    EXPECT_NE(fill.find(R"("fill_qty":40)"), std::string::npos);
    EXPECT_NE(fill.find(R"("fill_price":100.02)"), std::string::npos);
    EXPECT_NE(fill.find(R"("remaining_qty":60)"), std::string::npos);
    const std::string md = encodeMarketData("SIM1", true, 9998, true, 10002, false, 0, "t");
    EXPECT_NE(md.find(R"("best_bid":99.98)"), std::string::npos);
    EXPECT_NE(md.find(R"("last_trade":null)"), std::string::npos);
}

TEST(Protocol, TicksToDecimal) {
    EXPECT_EQ(ticksToDecimal(10002), "100.02");
    EXPECT_EQ(ticksToDecimal(10000), "100.00");
    EXPECT_EQ(ticksToDecimal(5), "0.05");
}

TEST(Protocol, JsonEscapesClientOrderId) {
    // A client_order_id with a quote must not break the framing/JSON.
    const std::string ack = encodeAck(R"(a"b)", "e-1", "t");
    EXPECT_NE(ack.find(R"("client_order_id":"a\"b")"), std::string::npos);
}

// ─────────────────────────────────────────────────────────────────────────────
// Framing
// ─────────────────────────────────────────────────────────────────────────────

TEST(Framing, RoundTripsMultipleFramesAcrossFeeds) {
    const std::string f1 = encodeFrame("{\"a\":1}");
    const std::string f2 = encodeFrame("{\"b\":2}");
    std::string stream = f1 + f2;

    FrameDecoder dec;
    // Feed one byte at a time to exercise partial-frame buffering.
    std::string payload;
    std::vector<std::string> got;
    for (char c : stream) {
        dec.feed(&c, 1);
        while (dec.next(payload)) {
            got.push_back(payload);
        }
    }
    ASSERT_EQ(got.size(), 2u);
    EXPECT_EQ(got[0], "{\"a\":1}");
    EXPECT_EQ(got[1], "{\"b\":2}");
    EXPECT_FALSE(dec.error());
}

TEST(Framing, BadMagicIsAStickyError) {
    FrameDecoder dec;
    const char junk[8] = {'X', 'X', 'X', 'X', 0, 0, 0, 1};
    dec.feed(junk, sizeof(junk));
    std::string payload;
    EXPECT_FALSE(dec.next(payload));
    EXPECT_TRUE(dec.error());
}

TEST(Framing, OverLengthIsRejected) {
    FrameDecoder dec;
    // Valid magic, but a length above the 1 MiB cap.
    char hdr[8] = {0x56, 0x45, 0x4E, 0x31, 0x7F, char(0xFF), char(0xFF), char(0xFF)};
    dec.feed(hdr, sizeof(hdr));
    std::string payload;
    EXPECT_FALSE(dec.next(payload));
    EXPECT_TRUE(dec.error());
}

// ─────────────────────────────────────────────────────────────────────────────
// Venue behaviour via a capturing send seam
// ─────────────────────────────────────────────────────────────────────────────

class VenueTest : public ::testing::Test {
protected:
    std::vector<std::pair<Venue::ConnId, std::string>> sent_;

    Venue makeVenue(std::size_t softCap = 1024) {
        return Venue(softCap + 64, softCap, [this](Venue::ConnId id, std::string msg) {
            sent_.emplace_back(id, std::move(msg));
        });
    }
    int count(Venue::ConnId conn, const std::string& needle) const {
        int n = 0;
        for (const auto& [id, msg] : sent_) {
            if (id == conn && msg.find(needle) != std::string::npos) {
                ++n;
            }
        }
        return n;
    }
    static std::string newOrder(const std::string& cid, const std::string& side, const char* price,
                                int qty, const std::string& symbol = "SIM1") {
        return std::string(R"({"type":"NEW_ORDER","client_order_id":")") + cid + R"(","symbol":")" +
               symbol + R"(","side":")" + side + R"(","price":)" + price + R"(,"qty":)" +
               std::to_string(qty) + "}";
    }
    static std::string cancel(const std::string& cid) {
        return std::string(R"({"type":"CANCEL","client_order_id":")") + cid + R"("})";
    }
};

TEST_F(VenueTest, AcksValidRestingOrderWithExchangeId) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("o1", "SELL", "100.00", 10));
    EXPECT_EQ(count(1, R"("type":"ACK")"), 1);
    EXPECT_EQ(count(1, R"("exchange_order_id":"e-1")"), 1);
}

TEST_F(VenueTest, RejectReasons) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("o1", "SELL", "100.00", 0));                 // bad_qty
    v.handleLine(1, newOrder("o2", "SELL", "100.025", 5));               // bad_price (>2dp)
    v.handleLine(1, newOrder("o3", "SELL", "100.00", 5, "WRONG"));       // unknown_symbol
    EXPECT_EQ(count(1, "bad_qty"), 1);
    EXPECT_EQ(count(1, "bad_price"), 1);
    EXPECT_EQ(count(1, "unknown_symbol"), 1);
}

TEST_F(VenueTest, DuplicateClientOrderId) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("dup", "SELL", "100.00", 10));
    v.handleLine(1, newOrder("dup", "SELL", "101.00", 10));
    EXPECT_EQ(count(1, "duplicate_client_order_id"), 1);
}

TEST_F(VenueTest, SelfCrossRejected) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("s1", "SELL", "100.00", 10));
    v.handleLine(1, newOrder("s2", "BUY", "100.00", 10));
    EXPECT_EQ(count(1, "self_match"), 1);
}

TEST_F(VenueTest, CrossBetweenTwoParticipantsFillsBothWithRemaining) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("rest", "SELL", "100.00", 10));  // conn1 rests 10
    v.handleLine(2, newOrder("aggr", "BUY", "100.00", 4));    // conn2 buys 4
    EXPECT_EQ(count(2, R"("remaining_qty":0)"), 1);  // aggressor fully filled
    EXPECT_EQ(count(1, R"("remaining_qty":6)"), 1);  // resting has 6 left
    EXPECT_EQ(count(1, R"("fill_price":100.00)"), 1);
}

TEST_F(VenueTest, CapacityGuard) {
    auto v = makeVenue(/*softCap=*/1);
    v.handleLine(1, newOrder("o1", "SELL", "100.00", 10));
    v.handleLine(1, newOrder("o2", "SELL", "101.00", 10));
    EXPECT_EQ(count(1, R"("type":"ACK")"), 1);
    EXPECT_EQ(count(1, "venue_full"), 1);
}

TEST_F(VenueTest, CancelRestingReturnsCancelAck) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("o1", "SELL", "100.00", 10));
    v.handleLine(1, cancel("o1"));
    EXPECT_EQ(count(1, R"("type":"CANCEL_ACK")"), 1);
}

TEST_F(VenueTest, CancelUnknownReturnsNotFound) {
    auto v = makeVenue();
    v.handleLine(1, cancel("nope"));
    EXPECT_EQ(count(1, R"("reason":"not_found")"), 1);
}

TEST_F(VenueTest, CancelAlreadyFilled) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("rest", "SELL", "100.00", 10));
    v.handleLine(2, newOrder("aggr", "BUY", "100.00", 10));  // fully fills conn1's order
    v.handleLine(1, cancel("rest"));
    EXPECT_EQ(count(1, R"("reason":"already_filled")"), 1);
}

TEST_F(VenueTest, CancelAlreadyCancelled) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("o1", "SELL", "100.00", 10));
    v.handleLine(1, cancel("o1"));
    v.handleLine(1, cancel("o1"));
    EXPECT_EQ(count(1, R"("reason":"already_cancelled")"), 1);
}

TEST_F(VenueTest, DropConnectionRemovesRestingOrders) {
    auto v = makeVenue();
    v.handleLine(1, newOrder("o1", "SELL", "100.00", 10));
    v.dropConnection(1);
    v.handleLine(2, newOrder("o2", "BUY", "100.00", 10));
    EXPECT_EQ(count(2, R"("type":"FILL")"), 0);
    EXPECT_EQ(count(2, R"("type":"ACK")"), 1);
}
