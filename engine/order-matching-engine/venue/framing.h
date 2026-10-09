#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// [4-byte "VEN1"][4-byte big-endian payload length][JSON payload]

namespace venue {

inline constexpr uint32_t kFrameMagic = 0x56454E31u;  // "VEN1"
inline constexpr std::size_t kFrameHeaderBytes = 8;
inline constexpr uint32_t kMaxPayloadBytes = 1u << 20;  // 1 MiB

std::string encodeFrame(const std::string& payload);

// Framing errors are sticky; callers must close the connection.
class FrameDecoder {
public:
    void feed(const char* data, std::size_t n) { buf_.append(data, n); }

    // False means incomplete input or a framing error; check error().
    bool next(std::string& payload);

    bool error() const { return error_; }

private:
    std::string buf_;
    bool error_ = false;
};

}  // namespace venue
