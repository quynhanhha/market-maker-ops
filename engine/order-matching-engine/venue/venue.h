#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "order_book.h"
#include "protocol.h"

// The Venue wraps the matching engine and owns all protocol-level state the
// engine deliberately does not provide: participant identity, ACK/REJECT,
// order validation, FILL routing, a capacity guard, and a live-order table
// (which also serves as the reconciliation substrate in later phases).
//
// It touches the matching engine ONLY through its public API — zero changes to
// the hot path. Order responses (ACK/REJECT/FILL) are delivered via the SendFn
// seam; the feed messages (HEARTBEAT/MARKET_DATA) are the server's job, built
// from currentTop().
namespace venue {

class Venue {
public:
    using ConnId = uint64_t;
    // Enqueue one already-encoded line to a specific connection.
    using SendFn = std::function<void(ConnId, std::string)>;

    // bookCapacity: OrderPool size (sized above softCap with margin for the
    // transient incoming order). softCap: max simultaneous live orders the venue
    // will admit before returning REJECT/VENUE_FULL — this is what prevents the
    // engine's unchecked pool-exhaustion crash.
    Venue(std::size_t bookCapacity, std::size_t softCap, SendFn send);

    // Dispatch one inbound protocol line from a connection.
    void handleLine(ConnId conn, const std::string& line);

    // Cancel and forget every resting order owned by a departing connection.
    void dropConnection(ConnId conn);

    struct Top {
        bool hasBid = false;
        uint32_t bidPx = 0;
        uint32_t bidQty = 0;
        bool hasAsk = false;
        uint32_t askPx = 0;
        uint32_t askQty = 0;
        bool operator==(const Top& o) const {
            return hasBid == o.hasBid && bidPx == o.bidPx && bidQty == o.bidQty &&
                   hasAsk == o.hasAsk && askPx == o.askPx && askQty == o.askQty;
        }
        bool operator!=(const Top& o) const { return !(*this == o); }
    };
    Top currentTop() const;

private:
    struct LiveOrder {
        ConnId owner;
        OrderSide side;
        uint32_t price;
        uint32_t remaining;
    };

    void handleNewOrder(ConnId conn, const InboundMessage& m);
    void handleCancel(ConnId conn, const InboundMessage& m);
    void onTrade(const Trade& t);
    void routeFill(uint64_t orderId, uint32_t price, uint32_t quantity);
    bool wouldSelfCross(ConnId conn, OrderSide side, uint32_t price) const;

    std::size_t softCap_;
    SendFn send_;
    std::unordered_map<uint64_t, LiveOrder> live_;
    OrderBook<std::function<void(const Trade&)>> book_;
};

}  // namespace venue
