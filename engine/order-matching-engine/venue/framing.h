#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Wire framing for the venue protocol:
//   [4-byte magic 0x56454E31 "VEN1"][4-byte big-endian uint32 payload length][JSON payload]

namespace venue {

inline constexpr uint32_t kFrameMagic = 0x56454E31u;  // "VEN1"
inline constexpr std::size_t kFrameHeaderBytes = 8;
// Guards against a corrupt/hostile length prefix demanding a huge allocation.
inline constexpr uint32_t kMaxPayloadBytes = 1u << 20;  // 1 MiB

// Wrap a JSON payload in a complete frame.
std::string encodeFrame(const std::string& payload);

// Streaming decoder: append received bytes, then pull complete payloads out one
// at a time. A framing violation (bad magic or over-length) is sticky and fatal
// for the connection — the caller should close it.
class FrameDecoder {
public:
    void feed(const char* data, std::size_t n) { buf_.append(data, n); }

    // Returns true and sets `payload` when a full frame is available. Returns
    // false when more bytes are needed (or when a framing error has occurred —
    // check error()).
    bool next(std::string& payload);

    bool error() const { return error_; }

private:
    std::string buf_;
    bool error_ = false;
};

}  // namespace venue
