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
      byEngineId_(),
      byConn_(),
      book_(bookCapacity, [this](const Trade& t) { onTrade(t); }) {}

void Venue::handleLine(ConnId conn, const std::string& payload) {
    const InboundMessage m = parseInbound(payload);
    if (m.parseError || m.type == MsgType::Unknown) {
        send_(conn, encodeReject(m.hasClientOrderId ? m.clientOrderId : "", "malformed",
                                 isoTimestampNow()));
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
            break;  
        case MsgType::Unknown:
            send_(conn, encodeReject(m.hasClientOrderId ? m.clientOrderId : "", "unsupported",
                                     isoTimestampNow()));
            break;
    }
}

void Venue::handleNewOrder(ConnId conn, const InboundMessage& m) {
    const std::string ts = isoTimestampNow();
    if (!m.hasClientOrderId) {
        send_(conn, encodeReject("", "missing_client_order_id", ts));
        return;
    }
    const std::string& cid = m.clientOrderId;
    if (!m.hasSymbol || m.symbol != kSymbol) {
        send_(conn, encodeReject(cid, "unknown_symbol", ts));
        return;
    }
    if (!m.hasSide) {
        send_(conn, encodeReject(cid, "bad_side", ts));
        return;
    }
    if (!m.hasPrice) {
        send_(conn, encodeReject(cid, "bad_price", ts));
        return;
    }
    if (!m.hasQty || m.qty == 0) {
        send_(conn, encodeReject(cid, "bad_qty", ts));
        return;
    }
    auto& connMap = byConn_[conn];
    if (connMap.find(cid) != connMap.end()) {
        send_(conn, encodeReject(cid, "duplicate_client_order_id", ts));
        return;
    }
    if (byEngineId_.size() >= softCap_) {
        send_(conn, encodeReject(cid, "venue_full", ts));
        return;
    }
    // Reject-on-self-cross (self-trade prevention). Stricter than the engine's
    // cancel-remainder SMP, but it keeps tables exact and means the engine's
    // SMP branch is never exercised.
    if (wouldSelfCross(conn, m.side, m.priceTicks)) {
        send_(conn, encodeReject(cid, "self_match", ts));
        return;
    }

    const uint64_t engineId = nextEngineId_++;
    const std::string exchangeId = "e-" + std::to_string(engineId);
    byEngineId_[engineId] = LiveOrder{conn, cid, exchangeId, m.side, m.priceTicks, m.qty};
    connMap[cid] = ClientRecord{OrderState::Resting, exchangeId, engineId};
    send_(conn, encodeAck(cid, exchangeId, ts));  // accepted; fills (if any) follow
    book_.addLimitOrder(toEngineSide(m.side), m.priceTicks, m.qty, engineId, conn);
}

void Venue::handleCancel(ConnId conn, const InboundMessage& m) {
    const std::string ts = isoTimestampNow();
    const std::string& cid = m.clientOrderId;
    if (!m.hasClientOrderId) {
        send_(conn, encodeCancelReject("", "not_found", ts));
        return;
    }
    auto connIt = byConn_.find(conn);
    if (connIt == byConn_.end()) {
        send_(conn, encodeCancelReject(cid, "not_found", ts));
        return;
    }
    auto recIt = connIt->second.find(cid);
    if (recIt == connIt->second.end()) {
        send_(conn, encodeCancelReject(cid, "not_found", ts));
        return;
    }
    ClientRecord& rec = recIt->second;
    if (rec.state == OrderState::Filled) {
        send_(conn, encodeCancelReject(cid, "already_filled", ts));
        return;
    }
    if (rec.state == OrderState::Cancelled) {
        send_(conn, encodeCancelReject(cid, "already_cancelled", ts));
        return;
    }
    book_.cancelOrder(rec.engineId);
    byEngineId_.erase(rec.engineId);
    rec.state = OrderState::Cancelled;
    send_(conn, encodeCancelAck(cid, rec.exchangeId, ts));
}

void Venue::onTrade(const Trade& t) {
    lastTradeTicks_ = t.price;
    hasLastTrade_ = true;
    routeFill(t.buyOrderId, t.price, t.quantity);
    routeFill(t.sellOrderId, t.price, t.quantity);
}

void Venue::routeFill(uint64_t engineId, uint32_t priceTicks, uint32_t qty) {
    auto it = byEngineId_.find(engineId);
    if (it == byEngineId_.end()) {
        return;  // defensive: every matched order should be in the table
    }
    LiveOrder& lo = it->second;
    const uint32_t remainingAfter = (lo.remaining > qty) ? (lo.remaining - qty) : 0u;
    send_(lo.owner, encodeFill(lo.exchangeId, lo.clientOrderId, qty, priceTicks, remainingAfter,
                               isoTimestampNow()));
    if (remainingAfter == 0) {
        auto cm = byConn_.find(lo.owner);
        if (cm != byConn_.end()) {
            auto r = cm->second.find(lo.clientOrderId);
            if (r != cm->second.end()) {
                r->second.state = OrderState::Filled;
            }
        }
        byEngineId_.erase(it);
    } else {
        lo.remaining = remainingAfter;
    }
}

bool Venue::wouldSelfCross(ConnId conn, OrderSide side, uint32_t priceTicks) const {
    for (const auto& [id, lo] : byEngineId_) {
        (void)id;
        if (lo.owner != conn) {
            continue;
        }
        if (side == OrderSide::Buy && lo.side == OrderSide::Sell && priceTicks >= lo.priceTicks) {
            return true;
        }
        if (side == OrderSide::Sell && lo.side == OrderSide::Buy && priceTicks <= lo.priceTicks) {
            return true;
        }
    }
    return false;
}

void Venue::dropConnection(ConnId conn) {
    std::vector<uint64_t> owned;
    for (const auto& [id, lo] : byEngineId_) {
        if (lo.owner == conn) {
            owned.push_back(id);
        }
    }
    for (const uint64_t id : owned) {
        book_.cancelOrder(id);
        byEngineId_.erase(id);
    }
    byConn_.erase(conn);
}

Venue::Top Venue::currentTop() const {
    Top t;
    if (const PriceLevel* b = book_.bestBid()) {
        t.hasBid = true;
        t.bidTicks = b->price;
    }
    if (const PriceLevel* a = book_.bestAsk()) {
        t.hasAsk = true;
        t.askTicks = a->price;
    }
    t.hasLast = hasLastTrade_;
    t.lastTicks = lastTradeTicks_;
    return t;
}

}  // namespace venue
