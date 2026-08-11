#include "protocol.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>

namespace venue {

static_assert(kPriceScale == 100, "ticksToDecimal formatting assumes 2 decimal places");

namespace {

struct JsonValue {
    bool isString = false;
    std::string str;      // decoded, for strings
    std::string numRaw;   // raw token, for numbers
};

void skipWs(const std::string& s, std::size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) {
        ++i;
    }
}

bool parseString(const std::string& s, std::size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') {
        return false;
    }
    ++i;
    out.clear();
    while (i < s.size()) {
        char c = s[i++];
        if (c == '"') {
            return true;
        }
        if (c == '\\') {
            if (i >= s.size()) {
                return false;
            }
            char e = s[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 't': out.push_back('\t'); break;
                case 'r': out.push_back('\r'); break;
                default: return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;
}

// Captures a JSON number token (optional '-', digits, optional '.digits'); no
// exponent support (unneeded). Interpretation (integer vs decimal) is deferred.
bool parseNumber(const std::string& s, std::size_t& i, std::string& raw) {
    const std::size_t start = i;
    if (i < s.size() && s[i] == '-') {
        ++i;
    }
    const std::size_t intStart = i;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        ++i;
    }
    if (i == intStart) {
        return false;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        const std::size_t fracStart = i;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            ++i;
        }
        if (i == fracStart) {
            return false;
        }
    }
    raw = s.substr(start, i - start);
    return true;
}

bool parseFlatObject(const std::string& s, std::map<std::string, JsonValue>& out) {
    std::size_t i = 0;
    skipWs(s, i);
    if (i >= s.size() || s[i] != '{') {
        return false;
    }
    ++i;
    skipWs(s, i);
    if (i < s.size() && s[i] == '}') {
        ++i;
        skipWs(s, i);
        return i >= s.size();
    }
    while (true) {
        skipWs(s, i);
        std::string key;
        if (!parseString(s, i, key)) {
            return false;
        }
        skipWs(s, i);
        if (i >= s.size() || s[i] != ':') {
            return false;
        }
        ++i;
        skipWs(s, i);
        if (i >= s.size()) {
            return false;
        }
        JsonValue v;
        if (s[i] == '"') {
            if (!parseString(s, i, v.str)) {
                return false;
            }
            v.isString = true;
        } else {
            if (!parseNumber(s, i, v.numRaw)) {
                return false;
            }
            v.isString = false;
        }
        out[key] = v;
        skipWs(s, i);
        if (i >= s.size()) {
            return false;
        }
        if (s[i] == ',') {
            ++i;
            continue;
        }
        if (s[i] == '}') {
            ++i;
            break;
        }
        return false;
    }
    skipWs(s, i);
    return i >= s.size();
}

bool parseUint32(const std::string& raw, uint32_t& out) {
    if (raw.empty()) {
        return false;
    }
    uint64_t v = 0;
    for (const char c : raw) {
        if (c < '0' || c > '9') {
            return false;  // rejects '-' and '.' too
        }
        v = v * 10 + static_cast<uint64_t>(c - '0');
        if (v > 0xFFFFFFFFull) {
            return false;
        }
    }
    out = static_cast<uint32_t>(v);
    return true;
}

// "100.02" -> 10002 ticks. Rejects negatives and >2 decimal places (→ bad_price).
bool parseDecimalToTicks(const std::string& raw, uint32_t& out) {
    if (raw.empty() || raw[0] == '-') {
        return false;
    }
    const std::size_t dot = raw.find('.');
    std::string intPart = (dot == std::string::npos) ? raw : raw.substr(0, dot);
    std::string fracPart = (dot == std::string::npos) ? std::string() : raw.substr(dot + 1);
    if (intPart.empty() || fracPart.size() > 2) {
        return false;
    }
    while (fracPart.size() < 2) {
        fracPart.push_back('0');
    }
    for (const char c : intPart) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    for (const char c : fracPart) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    uint64_t whole = 0;
    for (const char c : intPart) {
        whole = whole * 10 + static_cast<uint64_t>(c - '0');
        if (whole > 0xFFFFFFFFull) {
            return false;
        }
    }
    const uint64_t frac =
        static_cast<uint64_t>(fracPart[0] - '0') * 10 + static_cast<uint64_t>(fracPart[1] - '0');
    const uint64_t ticks = whole * kPriceScale + frac;
    if (ticks > 0xFFFFFFFFull) {
        return false;
    }
    out = static_cast<uint32_t>(ticks);
    return true;
}

std::string quoteJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (const char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof(b), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
    return out;
}

}  // namespace

InboundMessage parseInbound(const std::string& line) {
    InboundMessage m;
    std::map<std::string, JsonValue> obj;
    if (!parseFlatObject(line, obj)) {
        m.parseError = true;
        m.errorDetail = "json";
        return m;
    }
    auto typeIt = obj.find("type");
    if (typeIt == obj.end() || !typeIt->second.isString) {
        m.parseError = true;
        m.errorDetail = "type";
        return m;
    }
    const std::string& t = typeIt->second.str;
    if (t == "NEW_ORDER") {
        m.type = MsgType::NewOrder;
    } else if (t == "CANCEL") {
        m.type = MsgType::Cancel;
    } else if (t == "HEARTBEAT") {
        m.type = MsgType::Heartbeat;
    } else {
        m.type = MsgType::Unknown;
    }

    if (auto it = obj.find("client_order_id"); it != obj.end() && it->second.isString) {
        m.clientOrderId = it->second.str;
        m.hasClientOrderId = true;
    }
    if (auto it = obj.find("symbol"); it != obj.end() && it->second.isString) {
        m.symbol = it->second.str;
        m.hasSymbol = true;
    }
    if (auto it = obj.find("side"); it != obj.end() && it->second.isString) {
        if (it->second.str == "BUY") {
            m.side = OrderSide::Buy;
            m.hasSide = true;
        } else if (it->second.str == "SELL") {
            m.side = OrderSide::Sell;
            m.hasSide = true;
        }
    }
    if (auto it = obj.find("price"); it != obj.end() && !it->second.isString) {
        uint32_t ticks = 0;
        if (parseDecimalToTicks(it->second.numRaw, ticks)) {
            m.priceTicks = ticks;
            m.hasPrice = true;
        }
    }
    if (auto it = obj.find("qty"); it != obj.end() && !it->second.isString) {
        uint32_t q = 0;
        if (parseUint32(it->second.numRaw, q)) {
            m.qty = q;
            m.hasQty = true;
        }
    }
    return m;
}

const char* sideToString(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string ticksToDecimal(uint32_t ticks) {
    const uint32_t whole = ticks / kPriceScale;
    const uint32_t frac = ticks % kPriceScale;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%02u", whole, frac);
    return std::string(buf);
}

std::string isoTimestampNow() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec,
                  static_cast<int>(ms));
    return std::string(buf);
}

std::string encodeAck(const std::string& clientOrderId, const std::string& exchangeOrderId,
                      const std::string& ts) {
    return "{\"type\":\"ACK\",\"client_order_id\":" + quoteJson(clientOrderId) +
           ",\"exchange_order_id\":" + quoteJson(exchangeOrderId) +
           ",\"ts\":" + quoteJson(ts) + "}";
}

std::string encodeReject(const std::string& clientOrderId, const std::string& reason,
                         const std::string& ts) {
    return "{\"type\":\"REJECT\",\"client_order_id\":" + quoteJson(clientOrderId) +
           ",\"reason\":" + quoteJson(reason) + ",\"ts\":" + quoteJson(ts) + "}";
}

std::string encodeCancelAck(const std::string& clientOrderId, const std::string& exchangeOrderId,
                            const std::string& ts) {
    return "{\"type\":\"CANCEL_ACK\",\"client_order_id\":" + quoteJson(clientOrderId) +
           ",\"exchange_order_id\":" + quoteJson(exchangeOrderId) +
           ",\"ts\":" + quoteJson(ts) + "}";
}

std::string encodeCancelReject(const std::string& clientOrderId, const std::string& reason,
                               const std::string& ts) {
    return "{\"type\":\"CANCEL_REJECT\",\"client_order_id\":" + quoteJson(clientOrderId) +
           ",\"reason\":" + quoteJson(reason) + ",\"ts\":" + quoteJson(ts) + "}";
}

std::string encodeFill(const std::string& exchangeOrderId, const std::string& clientOrderId,
                       uint32_t fillQty, uint32_t fillPriceTicks, uint32_t remainingQty,
                       const std::string& ts) {
    return "{\"type\":\"FILL\",\"exchange_order_id\":" + quoteJson(exchangeOrderId) +
           ",\"client_order_id\":" + quoteJson(clientOrderId) +
           ",\"fill_qty\":" + std::to_string(fillQty) +
           ",\"fill_price\":" + ticksToDecimal(fillPriceTicks) +
           ",\"remaining_qty\":" + std::to_string(remainingQty) +
           ",\"ts\":" + quoteJson(ts) + "}";
}

std::string encodeHeartbeat(uint64_t seq, const std::string& ts) {
    return "{\"type\":\"HEARTBEAT\",\"seq\":" + std::to_string(seq) +
           ",\"ts\":" + quoteJson(ts) + "}";
}

std::string encodeMarketData(const std::string& symbol, bool hasBid, uint32_t bidTicks,
                             bool hasAsk, uint32_t askTicks, bool hasLast, uint32_t lastTicks,
                             const std::string& ts) {
    std::string s = "{\"type\":\"MARKET_DATA\",\"symbol\":" + quoteJson(symbol);
    s += ",\"best_bid\":";
    s += hasBid ? ticksToDecimal(bidTicks) : "null";
    s += ",\"best_ask\":";
    s += hasAsk ? ticksToDecimal(askTicks) : "null";
    s += ",\"last_trade\":";
    s += hasLast ? ticksToDecimal(lastTicks) : "null";
    s += ",\"ts\":" + quoteJson(ts) + "}";
    return s;
}

}  // namespace venue
