#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "server.h"

namespace {

void usage(const char* prog) {
    std::fprintf(stderr,
                 "usage: %s [--port N] [--capacity N] [--heartbeat-ms N]\n"
                 "  --port         TCP port to listen on         (default 9001)\n"
                 "  --capacity     max simultaneous live orders  (default 1048576)\n"
                 "  --heartbeat-ms heartbeat/market-data cadence  (default 1000)\n",
                 prog);
}

bool parseU64(const char* s, uint64_t& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s, &end, 10);
    if (end == nullptr || *end != '\0') {
        return false;
    }
    out = static_cast<uint64_t>(v);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    uint16_t port = 9001;
    uint64_t capacity = 1u << 20;  // max live (resting) orders admitted
    int heartbeatMs = 1000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](uint64_t& dst) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            return parseU64(argv[++i], dst);
        };
        if (arg == "--port") {
            uint64_t v = 0;
            if (!next(v) || v == 0 || v > 65535) {
                usage(argv[0]);
                return 2;
            }
            port = static_cast<uint16_t>(v);
        } else if (arg == "--capacity") {
            uint64_t v = 0;
            if (!next(v) || v == 0) {
                usage(argv[0]);
                return 2;
            }
            capacity = v;
        } else if (arg == "--heartbeat-ms") {
            uint64_t v = 0;
            if (!next(v) || v == 0 || v > 3600000) {
                usage(argv[0]);
                return 2;
            }
            heartbeatMs = static_cast<int>(v);
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    // Never let a peer disconnect take the process down via SIGPIPE.
    std::signal(SIGPIPE, SIG_IGN);

    // Size the pool above the admitted soft cap: headroom for the transient
    // incoming order plus a safety margin, so the engine's unchecked pool can
    // never be exhausted.
    const std::size_t softCap = static_cast<std::size_t>(capacity);
    const std::size_t bookCapacity = softCap + 64;

    venue::Server server(port, bookCapacity, softCap, heartbeatMs);
    return server.run();
}
