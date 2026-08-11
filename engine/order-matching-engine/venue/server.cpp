#include "server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <vector>

#include "framing.h"
#include "protocol.h"

namespace venue {

namespace {

bool setNonBlocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return false;
    }
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

std::string mdPayload(const Venue::Top& t) {
    return encodeMarketData(kSymbol, t.hasBid, t.bidTicks, t.hasAsk, t.askTicks, t.hasLast,
                            t.lastTicks, isoTimestampNow());
}

}  // namespace

Server::Server(uint16_t port, std::size_t bookCapacity, std::size_t softCap, int heartbeatMs)
    : port_(port),
      heartbeatMs_(heartbeatMs),
      venue_(bookCapacity, softCap,
             [this](Venue::ConnId id, std::string payload) { enqueue(id, std::move(payload)); }) {}

bool Server::setupListen() {
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) {
        std::perror("socket");
        return false;
    }
    const int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return false;
    }
    if (::listen(listenFd_, 64) < 0) {
        std::perror("listen");
        return false;
    }
    if (!setNonBlocking(listenFd_)) {
        std::perror("fcntl");
        return false;
    }
    std::fprintf(stderr, "venue listening on port %u (symbol %s)\n",
                 static_cast<unsigned>(port_), kSymbol);
    return true;
}

void Server::enqueue(Venue::ConnId id, std::string payload) {
    auto it = connFd_.find(id);
    if (it == connFd_.end()) {
        return;
    }
    auto cit = conns_.find(it->second);
    if (cit != conns_.end()) {
        cit->second.out += encodeFrame(payload);
    }
}

void Server::broadcastPayload(const std::string& payload) {
    const std::string framed = encodeFrame(payload);
    for (auto& [fd, c] : conns_) {
        (void)fd;
        c.out += framed;
    }
}

void Server::broadcastFeed(bool includeHeartbeat) {
    if (includeHeartbeat) {
        ++heartbeatSeq_;
        broadcastPayload(encodeHeartbeat(heartbeatSeq_, isoTimestampNow()));
    }
    lastTop_ = venue_.currentTop();
    broadcastPayload(mdPayload(lastTop_));
}

void Server::acceptNew() {
    while (true) {
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int cfd = ::accept(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len);
        if (cfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        setNonBlocking(cfd);
        const Venue::ConnId cid = nextConnId_++;
        conns_.emplace(cfd, Conn{cfd, FrameDecoder{}, std::string()});
        connFd_[cid] = cfd;
        fdConn_[cfd] = cid;
        // Immediate snapshot so a new observer has state without waiting for the tick.
        enqueue(cid, mdPayload(venue_.currentTop()));
    }
}

void Server::handleReadable(int fd) {
    char buf[4096];
    while (true) {
        const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            conns_[fd].decoder.feed(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0) {
            closeConn(fd);
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        closeConn(fd);
        return;
    }

    const Venue::ConnId cid = fdConn_[fd];
    std::string payload;
    while (conns_[fd].decoder.next(payload)) {
        venue_.handleLine(cid, payload);
    }
    if (conns_[fd].decoder.error()) {
        // Structural framing violation (bad magic / over-length) — fail loud.
        std::fprintf(stderr, "framing error on fd %d; closing\n", fd);
        closeConn(fd);
    }
}

void Server::handleWritable(int fd) {
    std::string& out = conns_[fd].out;
    while (!out.empty()) {
        const ssize_t n = ::send(fd, out.data(), out.size(), 0);
        if (n > 0) {
            out.erase(0, static_cast<std::size_t>(n));
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        closeConn(fd);
        return;
    }
}

void Server::closeConn(int fd) {
    auto fcit = fdConn_.find(fd);
    if (fcit != fdConn_.end()) {
        venue_.dropConnection(fcit->second);
        connFd_.erase(fcit->second);
        fdConn_.erase(fcit);
    }
    conns_.erase(fd);
    ::close(fd);
}

int Server::run() {
    if (!setupListen()) {
        return 1;
    }
    using namespace std::chrono;
    auto nextBeat = steady_clock::now() + milliseconds(heartbeatMs_);

    while (true) {
        std::vector<pollfd> pfds;
        pfds.reserve(conns_.size() + 1);
        pollfd lp{};
        lp.fd = listenFd_;
        lp.events = POLLIN;
        lp.revents = 0;
        pfds.push_back(lp);
        for (auto& [fd, c] : conns_) {
            pollfd p{};
            p.fd = fd;
            p.events = POLLIN;
            if (!c.out.empty()) {
                p.events = static_cast<short>(p.events | POLLOUT);
            }
            p.revents = 0;
            pfds.push_back(p);
        }

        const auto now = steady_clock::now();
        int timeoutMs;
        if (nextBeat > now) {
            const auto ms = duration_cast<milliseconds>(nextBeat - now).count();
            timeoutMs = (ms > heartbeatMs_) ? heartbeatMs_ : static_cast<int>(ms);
        } else {
            timeoutMs = 0;
        }

        const int ready = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::perror("poll");
            return 1;
        }

        if (steady_clock::now() >= nextBeat) {
            broadcastFeed(/*includeHeartbeat=*/true);
            nextBeat = steady_clock::now() + milliseconds(heartbeatMs_);
        }

        for (const pollfd& p : pfds) {
            if (p.revents == 0) {
                continue;
            }
            if (p.fd == listenFd_) {
                if (p.revents & POLLIN) {
                    acceptNew();
                }
                continue;
            }
            if (conns_.find(p.fd) == conns_.end()) {
                continue;  // closed earlier this pass
            }
            if (p.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                closeConn(p.fd);
                continue;
            }
            if (p.revents & POLLIN) {
                handleReadable(p.fd);
            }
            if (conns_.find(p.fd) != conns_.end() && (p.revents & POLLOUT)) {
                handleWritable(p.fd);
            }
        }

        // Coalesced market-data-on-change: emit once per loop if the top moved.
        const Venue::Top top = venue_.currentTop();
        if (top != lastTop_) {
            lastTop_ = top;
            broadcastPayload(mdPayload(top));
        }
    }
}

}  // namespace venue
