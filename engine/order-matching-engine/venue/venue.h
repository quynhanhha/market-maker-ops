#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

#include "order_book.h"
#include "protocol.h"

// The Venue wraps the matching engine and owns all protocol-level state the
// engine deliberately does not provide: participant identity, string client/
// exchange order ids, ACK/REJECT/CANCEL_ACK/CANCEL_REJECT, validation, FILL
// routing, a capacity guard, decimal↔tick conversion, and a live-order table
// (also the reconciliation substrate for later phases).
//
// It touches the matching engine ONLY through its public API — zero diff to the
// hot path. Order responses go out via the SendFn seam as JSON payloads (framing
// and the HEARTBEAT/MARKET_DATA feed are the server's job).
namespace venue {

inline constexpr const char* kSymbol = "SIM1";  // single hardcoded symbol for v1

class Venue {
public:
    using ConnId = uint64_t;
    using SendFn = std::function<void(ConnId, std::string)>;

    Venue(std::size_t bookCapacity, std::size_t softCap, SendFn send);

    void handleLine(ConnId conn, const std::string& payload);  // one JSON payload
    void dropConnection(ConnId conn);

    struct Top {
        bool hasBid = false;
        uint32_t bidTicks = 0;
        bool hasAsk = false;
        uint32_t askTicks = 0;
        bool hasLast = false;
        uint32_t lastTicks = 0;
        bool operator==(const Top& o) const {
            return hasBid == o.hasBid && bidTicks == o.bidTicks && hasAsk == o.hasAsk &&
                   askTicks == o.askTicks && hasLast == o.hasLast && lastTicks == o.lastTicks;
        }
        bool operator!=(const Top& o) const { return !(*this == o); }
    };
    Top currentTop() const;

private:
    enum class OrderState { Resting, Filled, Cancelled };

    struct LiveOrder {
        ConnId owner;
        std::string clientOrderId;
        std::string exchangeId;
        OrderSide side;
        uint32_t priceTicks;
        uint32_t remaining;
    };
    struct ClientRecord {
        OrderState state;
        std::string exchangeId;
        uint64_t engineId;
    };

    void handleNewOrder(ConnId conn, const InboundMessage& m);
    void handleCancel(ConnId conn, const InboundMessage& m);
    void onTrade(const Trade& t);
    void routeFill(uint64_t engineId, uint32_t priceTicks, uint32_t qty);
    bool wouldSelfCross(ConnId conn, OrderSide side, uint32_t priceTicks) const;

    std::size_t softCap_;
    SendFn send_;
    uint64_t nextEngineId_ = 1;  // engine id N ↔ exchange_order_id "e-N"
    bool hasLastTrade_ = false;
    uint32_t lastTradeTicks_ = 0;
    std::unordered_map<uint64_t, LiveOrder> byEngineId_;
    std::unordered_map<ConnId, std::unordered_map<std::string, ClientRecord>> byConn_;
    OrderBook<std::function<void(const Trade&)>> book_;
};

}  // namespace venue
