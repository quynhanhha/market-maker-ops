#pragma once

#include <cstdint>
#include <string>

namespace venue {

// Prices use integer cents internally; wire prices with >2 decimals are rejected.
inline constexpr uint32_t kPriceScale = 100;

enum class MsgType { NewOrder, Cancel, Heartbeat, Unknown };

enum class OrderSide { Buy, Sell };

// has* distinguishes valid fields from absent/invalid ones; malformed input sets parseError.
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
    bool hasPrice = false;
    bool hasQty = false;
    bool parseError = false;
    std::string errorDetail;
};

InboundMessage parseInbound(const std::string& line);

const char* sideToString(OrderSide side);
std::string ticksToDecimal(uint32_t ticks);
std::string isoTimestampNow();

// Encoders return unframed JSON; callers supply ISO8601 timestamps.
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
