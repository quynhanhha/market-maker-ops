#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <iostream>
#include <iomanip>
#include <random>
#include <cmath>

#ifdef __APPLE__
#include <mach/mach_time.h>
#endif

#include "order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// HIGH-RESOLUTION TIMER (platform-specific)
// ─────────────────────────────────────────────────────────────────────────────

class HighResTimer {
public:
    HighResTimer() {
#ifdef __APPLE__
        mach_timebase_info(&timebase_);
#endif
    }

    uint64_t now() const {
#ifdef __APPLE__
        return mach_absolute_time();
#else
        auto t = std::chrono::high_resolution_clock::now();
        return static_cast<uint64_t>(t.time_since_epoch().count());
#endif
    }

    // Convert ticks to nanoseconds
    uint64_t toNanos(uint64_t ticks) const {
#ifdef __APPLE__
        return ticks * timebase_.numer / timebase_.denom;
#else
        // Assuming high_resolution_clock uses nanoseconds
        return ticks;
#endif
    }

private:
#ifdef __APPLE__
    mach_timebase_info_data_t timebase_;
#endif
};

// ─────────────────────────────────────────────────────────────────────────────
// LATENCY COLLECTOR
// Using batch timing to avoid timer overhead artifacts
// ─────────────────────────────────────────────────────────────────────────────

class LatencyCollector {
public:
    explicit LatencyCollector(std::size_t capacity) {
        samples_.reserve(capacity);
    }

    void record(uint64_t nanos) {
        samples_.push_back(static_cast<int64_t>(nanos));
    }

    void reset() {
        samples_.clear();
    }

    std::size_t count() const { return samples_.size(); }

    void computeAndPrint(const std::string& label) {
        if (samples_.empty()) {
            std::cout << label << ": No samples\n";
            return;
        }

        std::sort(samples_.begin(), samples_.end());

        auto percentile = [this](double p) -> int64_t {
            std::size_t idx = static_cast<std::size_t>(p * static_cast<double>(samples_.size() - 1));
            return samples_[idx];
        };

        int64_t sum = std::accumulate(samples_.begin(), samples_.end(), int64_t{0});
        double mean = static_cast<double>(sum) / static_cast<double>(samples_.size());

        // Compute stddev
        double variance = 0.0;
        for (int64_t s : samples_) {
            double diff = static_cast<double>(s) - mean;
            variance += diff * diff;
        }
        variance /= static_cast<double>(samples_.size());
        double stddev = std::sqrt(variance);

        std::cout << "\n" << label << " (" << samples_.size() << " samples)\n";
        std::cout << std::string(60, '-') << "\n";
        std::cout << std::fixed << std::setprecision(1);
        std::cout << "  Min:    " << std::setw(10) << samples_.front() << " ns\n";
        std::cout << "  p50:    " << std::setw(10) << percentile(0.50) << " ns\n";
        std::cout << "  p90:    " << std::setw(10) << percentile(0.90) << " ns\n";
        std::cout << "  p99:    " << std::setw(10) << percentile(0.99) << " ns\n";
        std::cout << "  p99.9:  " << std::setw(10) << percentile(0.999) << " ns\n";
        std::cout << "  p99.99: " << std::setw(10) << percentile(0.9999) << " ns\n";
        std::cout << "  Max:    " << std::setw(10) << samples_.back() << " ns\n";
        std::cout << "  Mean:   " << std::setw(10) << mean << " ns\n";
        std::cout << "  Stddev: " << std::setw(10) << stddev << " ns\n";
    }

private:
    std::vector<int64_t> samples_;
};

// ─────────────────────────────────────────────────────────────────────────────
// INPUT GENERATION
// ─────────────────────────────────────────────────────────────────────────────

struct OrderInput {
    Side side;
    uint32_t price;
    uint32_t quantity;
    uint64_t id;
    uint64_t participantId;
};

std::vector<OrderInput> generateRestingOrders(std::size_t count, std::mt19937_64& rng) {
    std::vector<OrderInput> inputs;
    inputs.reserve(count);
    
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    std::uniform_int_distribution<uint32_t> priceDist(0, 9);
    
    for (std::size_t i = 0; i < count; ++i) {
        bool isBuy = (i % 2 == 0);
        uint32_t basePrice = isBuy ? 90 : 110;
        inputs.push_back({
            isBuy ? Side::Buy : Side::Sell,
            basePrice + priceDist(rng),
            qtyDist(rng),
            i + 1,
            1
        });
    }
    return inputs;
}

// ─────────────────────────────────────────────────────────────────────────────
// NO-OP CALLBACK
// ─────────────────────────────────────────────────────────────────────────────

inline void noOpCallback(const Trade&) {}

// ─────────────────────────────────────────────────────────────────────────────
// ESCAPE SINK (Prevent compiler from eliminating "unused" values)
// ─────────────────────────────────────────────────────────────────────────────

static volatile uint64_t g_sink = 0;

template<typename T>
inline void escape(T* p) {
    asm volatile("" : : "g"(p) : "memory");
}

inline void clobber() {
    asm volatile("" : : : "memory");
}

// ─────────────────────────────────────────────────────────────────────────────
// WARMUP
// ─────────────────────────────────────────────────────────────────────────────

void warmup() {
    OrderBook book(10000, noOpCallback);
    for (uint64_t i = 0; i < 5000; ++i) {
        book.addLimitOrder(Side::Buy, 100, 10, i, 1);
    }
    for (uint64_t i = 0; i < 5000; ++i) {
        book.cancelOrder(i);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK FUNCTIONS
// Using batch timing: time BATCH_SIZE ops, record total / BATCH_SIZE
// This avoids timer overhead dominating small operations
// ─────────────────────────────────────────────────────────────────────────────

constexpr std::size_t BATCH_SIZE = 100;

void benchmarkAddResting(std::size_t batches, const HighResTimer& timer) {
    std::mt19937_64 rng(42);
    LatencyCollector collector(batches);
    
    std::uniform_int_distribution<uint32_t> priceDist(90, 110);
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    
    OrderBook book(batches * BATCH_SIZE + 1000, noOpCallback);
    uint64_t id = 0;
    
    for (std::size_t batch = 0; batch < batches; ++batch) {
        uint64_t start = timer.now();
        
        for (std::size_t i = 0; i < BATCH_SIZE; ++i) {
            ++id;
            bool isBuy = (id % 2 == 0);
            uint32_t price = isBuy ? priceDist(rng) - 20 : priceDist(rng) + 20;  // non-crossing
            book.addLimitOrder(isBuy ? Side::Buy : Side::Sell, price, qtyDist(rng), id, id % 100);
        }
        
        clobber();
        uint64_t end = timer.now();
        
        uint64_t totalNanos = timer.toNanos(end - start);
        collector.record(totalNanos / BATCH_SIZE);
        
        // Cancel batch to keep book bounded
        for (std::size_t i = 0; i < BATCH_SIZE; ++i) {
            book.cancelOrder(id - i);
        }
    }
    
    collector.computeAndPrint("Add Resting Order (batched " + std::to_string(BATCH_SIZE) + " ops)");
}

void benchmarkAddCrossing(std::size_t batches, const HighResTimer& timer) {
    // This measures the FULL addLimitOrder() API call for a crossing order:
    // - Price level lookup
    // - Order matching (fills against resting)
    // - Trade callback invocation
    // - Order removal from price level if fully filled
    // - Hash index operations
    //
    // This is the end-to-end "add crossing order" latency as seen by the API caller.
    
    LatencyCollector collector(batches);
    constexpr std::size_t MATCH_BATCH = 100;
    
    // Pre-build book with resting sells
    OrderBook book(batches * MATCH_BATCH * 3 + 1000, noOpCallback);
    
    uint64_t restingId = 1;
    for (std::size_t i = 0; i < batches * MATCH_BATCH; ++i) {
        book.addLimitOrder(Side::Sell, 100, 1, restingId++, 1);
    }
    
    uint64_t matchId = restingId;
    
    for (std::size_t batch = 0; batch < batches; ++batch) {
        uint64_t start = timer.now();
        
        for (std::size_t i = 0; i < MATCH_BATCH; ++i) {
            // Aggressive buy that fully matches one resting sell
            book.addLimitOrder(Side::Buy, 100, 1, matchId++, 2);
        }
        
        clobber();
        uint64_t end = timer.now();
        
        uint64_t totalNanos = timer.toNanos(end - start);
        collector.record(totalNanos / MATCH_BATCH);
        
        // Replenish the batch (outside timing)
        for (std::size_t i = 0; i < MATCH_BATCH; ++i) {
            book.addLimitOrder(Side::Sell, 100, 1, restingId++, 1);
        }
    }
    
    collector.computeAndPrint("Add Crossing Order [full API] (batched " + std::to_string(MATCH_BATCH) + ")");
}

void benchmarkCancel(std::size_t batches, const HighResTimer& timer) {
    // Stable-state cancel benchmark:
    // - Book maintains constant size (~BOOK_SIZE orders)
    // - Each batch cancels CANCEL_BATCH random orders
    // - After timing, replenish those same orders
    // - This avoids book-draining artifacts
    
    std::mt19937_64 rng(42);
    LatencyCollector collector(batches);
    
    constexpr std::size_t CANCEL_BATCH = 100;
    constexpr std::size_t BOOK_SIZE = 10000;  // Stable book size
    
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    std::uniform_int_distribution<uint32_t> priceDist(0, 9);
    
    // Pre-populate book with BOOK_SIZE orders
    OrderBook book(BOOK_SIZE + CANCEL_BATCH + 1000, noOpCallback);
    std::vector<uint64_t> activeIds;
    activeIds.reserve(BOOK_SIZE);
    
    for (uint64_t id = 1; id <= BOOK_SIZE; ++id) {
        bool isBuy = (id % 2 == 0);
        uint32_t price = isBuy ? 90 + priceDist(rng) : 110 + priceDist(rng);
        book.addLimitOrder(isBuy ? Side::Buy : Side::Sell, price, qtyDist(rng), id, 1);
        activeIds.push_back(id);
    }
    
    uint64_t nextId = BOOK_SIZE + 1;
    
    for (std::size_t batch = 0; batch < batches; ++batch) {
        // Select CANCEL_BATCH random orders to cancel
        std::vector<uint64_t> toCancel;
        toCancel.reserve(CANCEL_BATCH);
        
        std::shuffle(activeIds.begin(), activeIds.end(), rng);
        for (std::size_t i = 0; i < CANCEL_BATCH && i < activeIds.size(); ++i) {
            toCancel.push_back(activeIds[i]);
        }
        
        // Time the cancellations
        uint64_t start = timer.now();
        
        for (uint64_t id : toCancel) {
            book.cancelOrder(id);
        }
        
        clobber();
        uint64_t end = timer.now();
        
        uint64_t totalNanos = timer.toNanos(end - start);
        collector.record(totalNanos / CANCEL_BATCH);
        
        // Replenish: remove cancelled IDs, add new orders (outside timing)
        for (uint64_t id : toCancel) {
            activeIds.erase(std::find(activeIds.begin(), activeIds.end(), id));
        }
        for (std::size_t i = 0; i < toCancel.size(); ++i) {
            bool isBuy = (nextId % 2 == 0);
            uint32_t price = isBuy ? 90 + priceDist(rng) : 110 + priceDist(rng);
            book.addLimitOrder(isBuy ? Side::Buy : Side::Sell, price, qtyDist(rng), nextId, 1);
            activeIds.push_back(nextId);
            ++nextId;
        }
    }
    
    collector.computeAndPrint("Cancel Order [stable-state] (batched " + std::to_string(CANCEL_BATCH) + ")");
}

void benchmarkMultiLevelSweep(std::size_t iterations, std::size_t numLevels, const HighResTimer& timer) {
    LatencyCollector collector(iterations);
    const auto sweepQty = static_cast<uint32_t>(numLevels * 10);
    
    // Pre-build book
    OrderBook book(numLevels * iterations * 2 + 1000, noOpCallback);
    
    uint64_t nextId = 1;
    for (std::size_t i = 0; i < numLevels; ++i) {
        book.addLimitOrder(Side::Sell, 100 + static_cast<uint32_t>(i), 10, nextId++, 1);
    }
    
    uint64_t sweepId = 1000000;
    
    for (std::size_t i = 0; i < iterations; ++i) {
        uint64_t start = timer.now();
        
        // Aggressive buy sweeps all levels
        book.addLimitOrder(Side::Buy, 100 + static_cast<uint32_t>(numLevels), 
                          sweepQty, sweepId++, 2);
        
        clobber();
        uint64_t end = timer.now();
        
        collector.record(timer.toNanos(end - start));
        
        // Replenish all levels
        for (std::size_t j = 0; j < numLevels; ++j) {
            book.addLimitOrder(Side::Sell, 100 + static_cast<uint32_t>(j), 10, nextId++, 1);
        }
    }
    
    collector.computeAndPrint("Multi-Level Sweep (" + std::to_string(numLevels) + " levels)");
}

void benchmarkBestBidAskAccess(std::size_t batches, const HighResTimer& timer) {
    // Best bid/ask access is O(1) - just returns pointer to vector.back()
    // Individual access is below timer resolution (~41ns), so we batch.
    // 
    // To prevent compiler from hoisting, we use volatile reads and 
    // modify the book BETWEEN batches (not inside timing).
    
    std::mt19937_64 rng(42);
    LatencyCollector collector(batches);
    
    std::uniform_int_distribution<uint32_t> priceDist(50, 80);
    constexpr std::size_t ACCESS_BATCH = 1000;
    
    // Build a reasonably populated book
    OrderBook book(batches + 10000, noOpCallback);
    auto inputs = generateRestingOrders(1000, rng);
    for (const auto& input : inputs) {
        book.addLimitOrder(input.side, input.price, input.quantity, input.id, input.participantId);
    }
    
    uint64_t id = 10000;
    volatile uint64_t sink = 0;  // Prevent optimization
    
    for (std::size_t batch = 0; batch < batches; ++batch) {
        uint64_t start = timer.now();
        
        for (std::size_t i = 0; i < ACCESS_BATCH; ++i) {
            auto* bid = book.bestBid();
            auto* ask = book.bestAsk();
            // Force the reads to happen
            if (bid) sink += bid->price;
            if (ask) sink += ask->price;
        }
        
        clobber();
        uint64_t end = timer.now();
        
        uint64_t totalNanos = timer.toNanos(end - start);
        // Report per-access-pair time (bid + ask = 1 "access")
        collector.record(totalNanos / ACCESS_BATCH);
        
        // Modify book between batches to prevent cross-batch hoisting
        book.addLimitOrder(Side::Buy, priceDist(rng), 1, id, 1);
        book.cancelOrder(id);
        ++id;
    }
    
    // Use sink to prevent DCE
    g_sink = sink;
    
    collector.computeAndPrint("Best Bid/Ask Access [batched " + std::to_string(ACCESS_BATCH) + " pairs]");
}

void benchmarkMixedWorkload(std::size_t iterations, const HighResTimer& timer) {
    std::mt19937_64 rng(42);
    LatencyCollector addCollector(iterations);
    LatencyCollector cancelCollector(iterations);
    LatencyCollector matchCollector(iterations);
    
    OrderBook book(iterations * 2, noOpCallback);
    
    std::uniform_int_distribution<int> opDist(1, 100);
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    std::uniform_int_distribution<uint32_t> priceDist(0, 9);
    
    uint64_t nextId = 1;
    std::vector<uint64_t> activeIds;
    activeIds.reserve(iterations);
    
    for (std::size_t i = 0; i < iterations; ++i) {
        int roll = opDist(rng);
        
        if (roll <= 70) {
            // Add resting (70%)
            bool isBuy = (nextId % 2 == 0);
            uint32_t price = isBuy ? 90 + priceDist(rng) : 110 + priceDist(rng);
            
            uint64_t start = timer.now();
            book.addLimitOrder(isBuy ? Side::Buy : Side::Sell, price, qtyDist(rng), nextId, 1);
            clobber();
            uint64_t end = timer.now();
            
            addCollector.record(timer.toNanos(end - start));
            activeIds.push_back(nextId);
            ++nextId;
            
        } else if (roll <= 90 && !activeIds.empty()) {
            // Cancel (20%)
            std::uniform_int_distribution<std::size_t> idxDist(0, activeIds.size() - 1);
            std::size_t idx = idxDist(rng);
            uint64_t id = activeIds[idx];
            
            uint64_t start = timer.now();
            book.cancelOrder(id);
            clobber();
            uint64_t end = timer.now();
            
            cancelCollector.record(timer.toNanos(end - start));
            activeIds.erase(activeIds.begin() + static_cast<std::ptrdiff_t>(idx));
            
        } else {
            // Add crossing (10%)
            bool isBuy = (nextId % 2 == 0);
            uint32_t price = isBuy ? 150 : 50;
            
            uint64_t start = timer.now();
            book.addLimitOrder(isBuy ? Side::Buy : Side::Sell, price, qtyDist(rng), nextId, 2);
            clobber();
            uint64_t end = timer.now();
            
            matchCollector.record(timer.toNanos(end - start));
            ++nextId;
        }
    }
    
    std::cout << "\n=== MIXED WORKLOAD BREAKDOWN ===";
    addCollector.computeAndPrint("Add (Resting)");
    cancelCollector.computeAndPrint("Cancel");
    matchCollector.computeAndPrint("Add (Crossing/Match)");
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN
// ─────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::size_t iterations = 10000;  // reduced for batched measurements
    
    if (argc > 1) {
        iterations = static_cast<std::size_t>(std::stoul(argv[1]));
    }
    
    HighResTimer timer;
    
    std::cout << "========================================\n";
    std::cout << "  LATENCY PERCENTILE REPORT\n";
    std::cout << "  Batches/Iterations: " << iterations << "\n";
#ifdef __APPLE__
    std::cout << "  Timer: mach_absolute_time\n";
    std::cout << "  Note: ~41ns resolution on Apple Silicon\n";
    std::cout << "  Batching used where single-op < 41ns\n";
#else
    std::cout << "  Timer: std::chrono::high_resolution_clock\n";
#endif
    std::cout << "========================================\n";
    
    warmup();
    
    benchmarkAddResting(iterations, timer);
    benchmarkAddCrossing(iterations, timer);
    benchmarkCancel(iterations, timer);
    benchmarkMultiLevelSweep(iterations / 10, 10, timer);
    benchmarkMultiLevelSweep(iterations / 100, 50, timer);
    benchmarkBestBidAskAccess(iterations, timer);
    benchmarkMixedWorkload(iterations, timer);
    
    std::cout << "\n========================================\n";
    std::cout << "  REPORT COMPLETE\n";
    std::cout << "========================================\n";
    
    return 0;
}
