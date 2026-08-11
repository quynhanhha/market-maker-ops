#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

#include "venue.h"

namespace venue {

// Single-threaded poll() TCP server. Owns all sockets and per-connection byte
// buffers; every engine call happens on this one thread (no locks). Accepts
// many connections, frames newline-delimited JSON, and broadcasts a periodic
// HEARTBEAT + MARKET_DATA feed on a timer (plus MARKET_DATA whenever the top of
// book changes).
class Server {
public:
    Server(uint16_t port, std::size_t bookCapacity, std::size_t softCap, int heartbeatMs);

    // Blocks running the event loop. Returns non-zero only on setup failure.
    int run();

private:
    struct Conn {
        int fd;
        std::string in;
        std::string out;
    };

    bool setupListen();
    void acceptNew();
    void handleReadable(int fd);
    void handleWritable(int fd);
    void processLines(int fd);
    void closeConn(int fd);
    void enqueue(Venue::ConnId id, std::string msg);
    void broadcast(const std::string& msg);
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
