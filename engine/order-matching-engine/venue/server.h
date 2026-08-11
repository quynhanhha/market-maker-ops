#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "framing.h"
#include "venue.h"

namespace venue {

// Single-threaded poll() TCP server. Owns all sockets and per-connection buffers;
// every engine call happens on this one thread (no locks). Frames inbound bytes
// with FrameDecoder, frames every outbound payload, and broadcasts a periodic
// HEARTBEAT + MARKET_DATA feed (plus MARKET_DATA when the top of book moves).
class Server {
public:
    Server(uint16_t port, std::size_t bookCapacity, std::size_t softCap, int heartbeatMs);

    int run();  // blocks; returns non-zero only on setup failure

private:
    struct Conn {
        int fd;
        FrameDecoder decoder;
        std::string out;
    };

    bool setupListen();
    void acceptNew();
    void handleReadable(int fd);
    void handleWritable(int fd);
    void closeConn(int fd);
    void enqueue(Venue::ConnId id, std::string payload);
    void broadcastPayload(const std::string& payload);
    void broadcastFeed(bool includeHeartbeat);

    uint16_t port_;
    int heartbeatMs_;
    int listenFd_ = -1;
    Venue::ConnId nextConnId_ = 1;
    uint64_t heartbeatSeq_ = 0;
    Venue::Top lastTop_;
    std::unordered_map<int, Conn> conns_;
    std::unordered_map<Venue::ConnId, int> connFd_;
    std::unordered_map<int, Venue::ConnId> fdConn_;
    Venue venue_;
};

}  // namespace venue
