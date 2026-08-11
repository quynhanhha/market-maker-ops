#include "framing.h"

namespace venue {

std::string encodeFrame(const std::string& payload) {
    const uint32_t len = static_cast<uint32_t>(payload.size());
    std::string frame;
    frame.reserve(kFrameHeaderBytes + payload.size());
    frame.push_back(static_cast<char>((kFrameMagic >> 24) & 0xFF));
    frame.push_back(static_cast<char>((kFrameMagic >> 16) & 0xFF));
    frame.push_back(static_cast<char>((kFrameMagic >> 8) & 0xFF));
    frame.push_back(static_cast<char>(kFrameMagic & 0xFF));
    frame.push_back(static_cast<char>((len >> 24) & 0xFF));
    frame.push_back(static_cast<char>((len >> 16) & 0xFF));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
    frame.append(payload);
    return frame;
}

bool FrameDecoder::next(std::string& payload) {
    if (error_) {
        return false;
    }
    if (buf_.size() < kFrameHeaderBytes) {
        return false;
    }

    const auto b = [this](std::size_t i) -> uint32_t {
        return static_cast<uint32_t>(static_cast<unsigned char>(buf_[i]));
    };
    const uint32_t magic = (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
    if (magic != kFrameMagic) {
        error_ = true;
        return false;
    }
    const uint32_t len = (b(4) << 24) | (b(5) << 16) | (b(6) << 8) | b(7);
    if (len > kMaxPayloadBytes) {
        error_ = true;
        return false;
    }
    if (buf_.size() < kFrameHeaderBytes + len) {
        return false;  // wait for the rest of the payload
    }

    payload.assign(buf_, kFrameHeaderBytes, len);
    buf_.erase(0, kFrameHeaderBytes + len);
    return true;
}

}  // namespace venue
