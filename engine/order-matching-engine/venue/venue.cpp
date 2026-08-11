#include "venue.h"

#include <string>
#include <vector>

namespace venue {

namespace {
Side toEngineSide(OrderSide s) {
    return s == OrderSide::Buy ? Side::Buy : Side::Sell;
}
}  // namespace

Venue::Venue(std::size_t bookCapacity, std::size_t softCap, SendFn send)
    : softCap_(softCap),
      send_(std::move(send)),
      live_(),
      book_(bookCapacity, [this](const Trade& t) { onTrade(t); }) {}

void Venue::handleLine(ConnId conn, const std::string& line) {
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
        return;  // blank keep-alive line
    }
    const InboundMessage m = parseInbound(line);
    if (m.parseError || m.type == MsgType::Unknown) {
        send_(conn, encodeReject(m.hasOrderId ? m.orderId : 0, "MALFORMED"));
        return;
    }
    switch (m.type) {
        case MsgType::NewOrder:
            handleNewOrder(conn, m);
            break;
        case MsgType::Cancel:
            handleCancel(conn, m);
            break;
        case MsgType::Heartbeat:
            break;  // client liveness ping — accepted, nothing to do
        case MsgType::Unknown:
            send_(conn, encodeReject(m.hasOrderId ? m.orderId : 0, "UNSUPPORTED"));
            break;
    }
}

void Venue::handleNewOrder(ConnId conn, const InboundMessage& m) {
    if (!m.hasOrderId) {
        send_(conn, encodeReject(0, "MISSING_ORDER_ID"));
        return;
    }
    if (!m.hasSide) {
        send_(conn, encodeReject(m.orderId, "INVALID_SIDE"));
        return;
    }
    if (!m.hasPrice || m.price == 0) {
        send_(conn, encodeReject(m.orderId, "INVALID_PRICE"));
        return;
    }
    if (!m.hasQuantity || m.quantity == 0) {
        send_(conn, encodeReject(m.orderId, "INVALID_QUANTITY"));
        return;
    }
    if (live_.find(m.orderId) != live_.end()) {
        send_(conn, encodeReject(m.orderId, "DUPLICATE_ORDER_ID"));
        return;
    }
    if (live_.size() >= softCap_) {
        send_(conn, encodeReject(m.orderId, "VENUE_FULL"));
        return;
    }
    // Reject-on-self-cross (self-trade prevention). Stricter than the engine's
    // cancel-remainder SMP, but it keeps the live-order table exact and means
    // the engine's SMP branch is never exercised (no phantom entries).
    if (wouldSelfCross(conn, m.side, m.price)) {
        send_(conn, encodeReject(m.orderId, "SELF_MATCH"));
        return;
    }

    // Record before submitting so onTrade() can route fills for the aggressor.
    live_[m.orderId] = LiveOrder{conn, m.side, m.price, m.quantity};
    send_(conn, encodeAck(m.orderId));  // accepted; fills (if any) follow
    book_.addLimitOrder(toEngineSide(m.side), m.price, m.quantity, m.orderId, conn);
}

void Venue::handleCancel(ConnId conn, const InboundMessage& m) {
    if (!m.hasOrderId) {
        send_(conn, encodeReject(0, "MISSING_ORDER_ID"));
        return;
    }
    auto it = live_.find(m.orderId);
    if (it == live_.end()) {
        send_(conn, encodeReject(m.orderId, "UNKNOWN_ORDER"));
        return;
    }
    if (it->second.owner != conn) {
        send_(conn, encodeReject(m.orderId, "NOT_OWNER"));
        return;
    }
    book_.cancelOrder(m.orderId);
    live_.erase(it);
    send_(conn, encodeAck(m.orderId));
}

void Venue::onTrade(const Trade& t) {
    routeFill(t.buyOrderId, t.price, t.quantity);
    routeFill(t.sellOrderId, t.price, t.quantity);
}

void Venue::routeFill(uint64_t orderId, uint32_t price, uint32_t quantity) {
    auto it = live_.find(orderId);
    if (it == live_.end()) {
        return;  // defensive: every matched order should be in the table
    }
    send_(it->second.owner, encodeFill(orderId, it->second.side, price, quantity));
    if (it->second.remaining <= quantity) {
        live_.erase(it);
    } else {
        it->second.remaining -= quantity;
    }
}

bool Venue::wouldSelfCross(ConnId conn, OrderSide side, uint32_t price) const {
    for (const auto& [id, lo] : live_) {
        (void)id;
        if (lo.owner != conn) {
            continue;
        }
        if (side == OrderSide::Buy && lo.side == OrderSide::Sell && price >= lo.price) {
            return true;
        }
        if (side == OrderSide::Sell && lo.side == OrderSide::Buy && price <= lo.price) {
            return true;
        }
    }
    return false;
}

void Venue::dropConnection(ConnId conn) {
    std::vector<uint64_t> owned;
    for (const auto& [id, lo] : live_) {
        if (lo.owner == conn) {
            owned.push_back(id);
        }
    }
    for (const uint64_t id : owned) {
        book_.cancelOrder(id);
        live_.erase(id);
    }
}

Venue::Top Venue::currentTop() const {
    Top t;
    if (const PriceLevel* b = book_.bestBid()) {
        t.hasBid = true;
        t.bidPx = b->price;
        t.bidQty = b->totalQuantity;
    }
    if (const PriceLevel* a = book_.bestAsk()) {
        t.hasAsk = true;
        t.askPx = a->price;
        t.askQty = a->totalQuantity;
    }
    return t;
}

}  // namespace venue
