#pragma once

#include <cstdint>
#include <string>

// Message layer for the exchange venue. Payloads are JSON objects (framing lives
// in framing.h). Prices are decimals on the wire and fixed-point integer *ticks*
// internally (kPriceScale ticks per unit). This module holds no book state.
namespace venue {

// 1 tick = 0.01 (two decimal places). >2 dp
// on the wire is rejected as bad_price rather than silently rounded.
inline constexpr uint32_t kPriceScale = 100;

enum class MsgType { NewOrder, Cancel, Heartbeat, Unknown };

enum class OrderSide { Buy, Sell };

// A parsed inbound line. Parsing never throws; malformed input sets parseError.
// has* flags distinguish "field absent/invalid" from "present" for precise REJECTs.
struct InboundMessage {
    MsgType type = MsgType::Unknown;
    std::string clientOrderId;
    std::string symbol;
    OrderSide side = OrderSide::Buy;
    uint32_t priceTicks = 0;
    uint32_t qty = 0;
    bool hasClientOrderId = false;
    bool hasSymbol = false;
    bool hasSide = false;
    bool hasPrice = false;  // present AND a valid ≤2-dp positive decimal
    bool hasQty = false;
    bool parseError = false;
    std::string errorDetail;
};

InboundMessage parseInbound(const std::string& line);

const char* sideToString(OrderSide side);
std::string ticksToDecimal(uint32_t ticks);   // 10002 -> "100.02"
std::string isoTimestampNow();                 // "2026-08-11T14:51:00.123Z"

// Encoders return a JSON payload (no framing, no trailing newline). Callers pass
// the ISO8601 `ts` so the encoders stay pure/testable.
std::string encodeAck(const std::string& clientOrderId, const std::string& exchangeOrderId,
                      const std::string& ts);
std::string encodeReject(const std::string& clientOrderId, const std::string& reason,
                         const std::string& ts);
std::string encodeCancelAck(const std::string& clientOrderId, const std::string& exchangeOrderId,
                            const std::string& ts);
std::string encodeCancelReject(const std::string& clientOrderId, const std::string& reason,
                               const std::string& ts);
std::string encodeFill(const std::string& exchangeOrderId, const std::string& clientOrderId,
                       uint32_t fillQty, uint32_t fillPriceTicks, uint32_t remainingQty,
                       const std::string& ts);
std::string encodeHeartbeat(uint64_t seq, const std::string& ts);
std::string encodeMarketData(const std::string& symbol, bool hasBid, uint32_t bidTicks,
                             bool hasAsk, uint32_t askTicks, bool hasLast, uint32_t lastTicks,
                             const std::string& ts);

}  // namespace venue
