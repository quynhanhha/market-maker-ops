#include "protocol.h"

#include <limits>
#include <map>

namespace venue {

namespace {

struct JsonValue {
    bool isString = false;
    std::string str;
    uint64_t num = 0;
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
                default: return false;  // unsupported escape
            }
        } else {
            out.push_back(c);
        }
    }
    return false;  // unterminated string
}

bool parseNumber(const std::string& s, std::size_t& i, uint64_t& out) {
    const std::size_t start = i;
    uint64_t val = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        const uint64_t d = static_cast<uint64_t>(s[i] - '0');
        if (val > (std::numeric_limits<uint64_t>::max() - d) / 10) {
            return false;  // overflow
        }
        val = val * 10 + d;
        ++i;
    }
    if (i == start) {
        return false;  // no digits
    }
    out = val;
    return true;
}

// Parses a flat JSON object of string/number values. Rejects nesting, arrays,
// booleans, and trailing garbage. Returns false on any malformed structure.
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
            if (!parseNumber(s, i, v.num)) {
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
    return i >= s.size();  // trailing garbage after '}' → malformed
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

    if (auto it = obj.find("order_id"); it != obj.end() && !it->second.isString) {
        m.orderId = it->second.num;
        m.hasOrderId = true;
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
        if (it->second.num <= std::numeric_limits<uint32_t>::max()) {
            m.price = static_cast<uint32_t>(it->second.num);
            m.hasPrice = true;
        }
    }
    if (auto it = obj.find("quantity"); it != obj.end() && !it->second.isString) {
        if (it->second.num <= std::numeric_limits<uint32_t>::max()) {
            m.quantity = static_cast<uint32_t>(it->second.num);
            m.hasQuantity = true;
        }
    }
    return m;
}

const char* sideToString(OrderSide side) {
    return side == OrderSide::Buy ? "BUY" : "SELL";
}

std::string encodeAck(uint64_t orderId) {
    return "{\"type\":\"ACK\",\"order_id\":" + std::to_string(orderId) +
           ",\"status\":\"ACCEPTED\"}\n";
}

std::string encodeReject(uint64_t orderId, const std::string& reason) {
    return "{\"type\":\"REJECT\",\"order_id\":" + std::to_string(orderId) +
           ",\"reason\":\"" + reason + "\"}\n";
}

std::string encodeFill(uint64_t orderId, OrderSide side, uint32_t price, uint32_t quantity) {
    return "{\"type\":\"FILL\",\"order_id\":" + std::to_string(orderId) +
           ",\"side\":\"" + sideToString(side) +
           "\",\"price\":" + std::to_string(price) +
           ",\"quantity\":" + std::to_string(quantity) + "}\n";
}

std::string encodeHeartbeat(uint64_t seq, uint64_t tsMillis) {
    return "{\"type\":\"HEARTBEAT\",\"seq\":" + std::to_string(seq) +
           ",\"ts\":" + std::to_string(tsMillis) + "}\n";
}

std::string encodeMarketData(bool hasBid, uint32_t bidPx, uint32_t bidQty,
                             bool hasAsk, uint32_t askPx, uint32_t askQty,
                             uint64_t tsMillis) {
    std::string s = "{\"type\":\"MARKET_DATA\"";
    if (hasBid) {
        s += ",\"bid_px\":" + std::to_string(bidPx) + ",\"bid_qty\":" + std::to_string(bidQty);
    } else {
        s += ",\"bid_px\":null,\"bid_qty\":null";
    }
    if (hasAsk) {
        s += ",\"ask_px\":" + std::to_string(askPx) + ",\"ask_qty\":" + std::to_string(askQty);
    } else {
        s += ",\"ask_px\":null,\"ask_qty\":null";
    }
    s += ",\"ts\":" + std::to_string(tsMillis) + "}\n";
    return s;
}

}  // namespace venue
