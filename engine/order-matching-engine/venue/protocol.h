#pragma once

#include <cstdint>
#include <string>

// Wire protocol for the exchange venue: newline-delimited JSON, one object per
// line. This module only encodes/decodes messages — it holds no book state.
namespace venue {

enum class MsgType { NewOrder, Cancel, Heartbeat, Unknown };

enum class OrderSide { Buy, Sell };

// A parsed inbound line. Parsing never throws; malformed input sets parseError.
// The has* flags distinguish "field absent/invalid" from "field present" so the
// venue can produce precise REJECT reasons.
struct InboundMessage {
    MsgType type = MsgType::Unknown;
    uint64_t orderId = 0;
    OrderSide side = OrderSide::Buy;
    uint32_t price = 0;
    uint32_t quantity = 0;
    bool hasOrderId = false;
    bool hasSide = false;
    bool hasPrice = false;
    bool hasQuantity = false;
    bool parseError = false;
    std::string errorDetail;
};

InboundMessage parseInbound(const std::string& line);

const char* sideToString(OrderSide side);

// Each encoder returns a single newline-terminated JSON line.
std::string encodeAck(uint64_t orderId);
std::string encodeReject(uint64_t orderId, const std::string& reason);
std::string encodeFill(uint64_t orderId, OrderSide side, uint32_t price, uint32_t quantity);
std::string encodeHeartbeat(uint64_t seq, uint64_t tsMillis);
std::string encodeMarketData(bool hasBid, uint32_t bidPx, uint32_t bidQty,
                             bool hasAsk, uint32_t askPx, uint32_t askQty,
                             uint64_t tsMillis);

}  // namespace venue
