#include <benchmark/benchmark.h>
#include <random>
#include <vector>
#include <algorithm>
#include <numeric>

#include "order_book.h"

// ─────────────────────────────────────────────────────────────────────────────
// INPUT GENERATORS (Pre-computed, no RNG in timed loops)
// ─────────────────────────────────────────────────────────────────────────────

struct OrderInput {
    Side side;
    uint32_t price;
    uint32_t quantity;
    uint64_t id;
    uint64_t participantId;
};

class InputGenerator {
public:
    explicit InputGenerator(uint64_t seed = 42) : rng_(seed) {}

    // Non-crossing orders that will rest in book
    std::vector<OrderInput> generateRestingOrders(std::size_t count, 
                                                   uint32_t bidStart = 90, 
                                                   uint32_t askStart = 110) {
        std::vector<OrderInput> inputs;
        inputs.reserve(count);
        
        std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
        std::uniform_int_distribution<uint32_t> priceDist(0, 9);
        std::uniform_int_distribution<uint64_t> partDist(1, 100);
        
        for (std::size_t i = 0; i < count; ++i) {
            bool isBuy = (i % 2 == 0);
            uint32_t basePrice = isBuy ? bidStart : askStart;
            inputs.push_back({
                isBuy ? Side::Buy : Side::Sell,
                basePrice + priceDist(rng_),
                qtyDist(rng_),
                i + 1,
                partDist(rng_)
            });
        }
        return inputs;
    }

    // Aggressive crossing orders
    std::vector<OrderInput> generateCrossingOrders(std::size_t count,
                                                    uint32_t crossPrice = 100) {
        std::vector<OrderInput> inputs;
        inputs.reserve(count);
        
        std::uniform_int_distribution<uint32_t> qtyDist(1, 50);
        std::uniform_int_distribution<uint64_t> partDist(101, 200);  // different participants
        
        for (std::size_t i = 0; i < count; ++i) {
            bool isBuy = (i % 2 == 0);
            // Buy at high price crosses asks, Sell at low price crosses bids
            uint32_t price = isBuy ? crossPrice + 50 : crossPrice - 50;
            inputs.push_back({
                isBuy ? Side::Buy : Side::Sell,
                price,
                qtyDist(rng_),
                100000 + i,
                partDist(rng_)
            });
        }
        return inputs;
    }

    // Cancel targets (order IDs to cancel)
    std::vector<uint64_t> generateCancelTargets(std::size_t count, uint64_t maxId) {
        std::vector<uint64_t> targets;
        targets.reserve(count);
        
        std::uniform_int_distribution<uint64_t> idDist(1, maxId);
        for (std::size_t i = 0; i < count; ++i) {
            targets.push_back(idDist(rng_));
        }
        return targets;
    }

    // Mixed workload: 70% add-rest, 20% cancel, 10% add-cross
    enum class OpType { AddRest, Cancel, AddCross };
    
    struct MixedOp {
        OpType type;
        OrderInput order;      // for Add*
        uint64_t cancelId;     // for Cancel
    };

    std::vector<MixedOp> generateMixedWorkload(std::size_t count) {
        std::vector<MixedOp> ops;
        ops.reserve(count);
        
        std::uniform_int_distribution<int> opDist(1, 100);
        std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
        std::uniform_int_distribution<uint32_t> priceDist(0, 9);
        std::uniform_int_distribution<uint64_t> partDist(1, 100);
        
        uint64_t nextId = 1;
        std::vector<uint64_t> activeIds;
        activeIds.reserve(count);
        
        for (std::size_t i = 0; i < count; ++i) {
            int roll = opDist(rng_);
            MixedOp op{};
            
            if (roll <= 70) {
                // Add resting (70%)
                op.type = OpType::AddRest;
                bool isBuy = (nextId % 2 == 0);
                op.order = {
                    isBuy ? Side::Buy : Side::Sell,
                    isBuy ? 90 + priceDist(rng_) : 110 + priceDist(rng_),
                    qtyDist(rng_),
                    nextId,
                    partDist(rng_)
                };
                activeIds.push_back(nextId);
                ++nextId;
            } else if (roll <= 90 && !activeIds.empty()) {
                // Cancel (20%)
                op.type = OpType::Cancel;
                std::uniform_int_distribution<std::size_t> idxDist(0, activeIds.size() - 1);
                std::size_t idx = idxDist(rng_);
                op.cancelId = activeIds[idx];
                activeIds.erase(activeIds.begin() + static_cast<std::ptrdiff_t>(idx));
            } else {
                // Add crossing (10%)
                op.type = OpType::AddCross;
                bool isBuy = (nextId % 2 == 0);
                op.order = {
                    isBuy ? Side::Buy : Side::Sell,
                    isBuy ? 150u : 50u,  // guaranteed to cross
                    qtyDist(rng_),
                    nextId,
                    partDist(rng_) + 200  // different participant
                };
                ++nextId;
            }
            ops.push_back(op);
        }
        return ops;
    }

private:
    std::mt19937_64 rng_;
};

// ─────────────────────────────────────────────────────────────────────────────
// NO-OP CALLBACK (minimal overhead)
// ─────────────────────────────────────────────────────────────────────────────

inline void noOpCallback(const Trade&) {}

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: ADD ONLY (Resting Orders)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_AddOnly_Resting(benchmark::State& state) {
    const auto numOrders = static_cast<std::size_t>(state.range(0));
    
    InputGenerator gen;
    auto inputs = gen.generateRestingOrders(numOrders);
    
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(numOrders + 100, noOpCallback);
        state.ResumeTiming();
        
        for (const auto& input : inputs) {
            book.addLimitOrder(input.side, input.price, input.quantity, 
                              input.id, input.participantId);
        }
        
        benchmark::DoNotOptimize(book.bestBid());
        benchmark::DoNotOptimize(book.bestAsk());
        benchmark::ClobberMemory();
    }
    
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(numOrders));
    state.SetLabel("orders/iter");
}

BENCHMARK(BM_AddOnly_Resting)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kMicrosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: MATCH HEAVY (Crossing Orders)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_MatchHeavy(benchmark::State& state) {
    const auto numResting = static_cast<std::size_t>(state.range(0));
    const auto numCrossing = numResting / 2;
    
    InputGenerator gen;
    auto restingInputs = gen.generateRestingOrders(numResting);
    auto crossingInputs = gen.generateCrossingOrders(numCrossing);
    
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(numResting + numCrossing + 100, noOpCallback);
        
        // Pre-populate book
        for (const auto& input : restingInputs) {
            book.addLimitOrder(input.side, input.price, input.quantity,
                              input.id, input.participantId);
        }
        state.ResumeTiming();
        
        // Timed: crossing orders that trigger matching
        for (const auto& input : crossingInputs) {
            book.addLimitOrder(input.side, input.price, input.quantity,
                              input.id, input.participantId);
        }
        
        benchmark::DoNotOptimize(book.bestBid());
        benchmark::DoNotOptimize(book.bestAsk());
        benchmark::ClobberMemory();
    }
    
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(numCrossing));
    state.SetLabel("crossing orders/iter");
}

BENCHMARK(BM_MatchHeavy)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kMicrosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: CANCEL ONLY
// ─────────────────────────────────────────────────────────────────────────────

static void BM_CancelOnly(benchmark::State& state) {
    const auto numOrders = static_cast<std::size_t>(state.range(0));
    
    InputGenerator gen;
    auto inputs = gen.generateRestingOrders(numOrders);
    
    // Shuffle cancel order for realistic access pattern
    std::vector<uint64_t> cancelOrder(numOrders);
    std::iota(cancelOrder.begin(), cancelOrder.end(), 1);
    std::shuffle(cancelOrder.begin(), cancelOrder.end(), std::mt19937_64(123));
    
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(numOrders + 100, noOpCallback);
        
        for (const auto& input : inputs) {
            book.addLimitOrder(input.side, input.price, input.quantity,
                              input.id, input.participantId);
        }
        state.ResumeTiming();
        
        // Timed: cancel all orders
        for (uint64_t id : cancelOrder) {
            book.cancelOrder(id);
        }
        
        benchmark::DoNotOptimize(book.bestBid());
        benchmark::ClobberMemory();
    }
    
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(numOrders));
    state.SetLabel("cancels/iter");
}

BENCHMARK(BM_CancelOnly)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kMicrosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: MIXED WORKLOAD (70% add-rest, 20% cancel, 10% add-cross)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_MixedWorkload(benchmark::State& state) {
    const auto numOps = static_cast<std::size_t>(state.range(0));
    
    InputGenerator gen;
    auto ops = gen.generateMixedWorkload(numOps);
    
    for (auto _ : state) {
        state.PauseTiming();
        OrderBook book(numOps + 100, noOpCallback);
        state.ResumeTiming();
        
        for (const auto& op : ops) {
            switch (op.type) {
                case InputGenerator::OpType::AddRest:
                case InputGenerator::OpType::AddCross:
                    book.addLimitOrder(op.order.side, op.order.price, 
                                      op.order.quantity, op.order.id,
                                      op.order.participantId);
                    break;
                case InputGenerator::OpType::Cancel:
                    book.cancelOrder(op.cancelId);
                    break;
            }
        }
        
        benchmark::DoNotOptimize(book.bestBid());
        benchmark::DoNotOptimize(book.bestAsk());
        benchmark::ClobberMemory();
    }
    
    state.SetItemsProcessed(state.iterations() * static_cast<int64_t>(numOps));
    state.SetLabel("ops/iter");
}

BENCHMARK(BM_MixedWorkload)
    ->Arg(1000)
    ->Arg(10000)
    ->Arg(100000)
    ->Unit(benchmark::kMicrosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: SINGLE ADD LATENCY (Microbenchmark)
// ─────────────────────────────────────────────────────────────────────────────

static void BM_SingleAdd_Empty(benchmark::State& state) {
    // Varying prices to prevent trivial branch prediction
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> priceDist(90, 110);
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    
    OrderBook book(100000, noOpCallback);
    uint64_t id = 0;
    
    for (auto _ : state) {
        ++id;
        uint32_t price = priceDist(rng);
        uint32_t qty = qtyDist(rng);
        Side side = (id % 2 == 0) ? Side::Buy : Side::Sell;
        
        book.addLimitOrder(side, price, qty, id, id % 100);
        
        // Force materialization of side effects
        benchmark::DoNotOptimize(book.bestBid());
        benchmark::ClobberMemory();
        
        // Cancel to keep book bounded (untimed but unavoidable)
        book.cancelOrder(id);
    }
    
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_SingleAdd_Empty)->Unit(benchmark::kNanosecond);

static void BM_SingleAdd_PopulatedBook(benchmark::State& state) {
    const auto bookDepth = static_cast<std::size_t>(state.range(0));
    
    InputGenerator gen;
    auto inputs = gen.generateRestingOrders(bookDepth);
    
    // Varying inputs to prevent prediction
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> priceDist(50, 80);  // non-crossing
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    
    OrderBook book(bookDepth + 10000, noOpCallback);
    for (const auto& input : inputs) {
        book.addLimitOrder(input.side, input.price, input.quantity,
                          input.id, input.participantId);
    }
    
    uint64_t id = bookDepth + 1;
    
    for (auto _ : state) {
        uint32_t price = priceDist(rng);
        uint32_t qty = qtyDist(rng);
        Side side = (id % 2 == 0) ? Side::Buy : Side::Sell;
        
        book.addLimitOrder(side, price, qty, id, id % 100);
        
        benchmark::DoNotOptimize(book.bestBid());
        benchmark::ClobberMemory();
        
        book.cancelOrder(id);
        ++id;
    }
    
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_SingleAdd_PopulatedBook)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: SINGLE MATCH LATENCY
// Fixed: Pre-build book, only time the matching operation
// ─────────────────────────────────────────────────────────────────────────────

static void BM_SingleMatch(benchmark::State& state) {
    const auto bookDepth = static_cast<std::size_t>(state.range(0));
    
    // We need fresh resting orders each iteration
    // Time: replenish + match cycle
    OrderBook book(bookDepth * 2 + 1000, noOpCallback);
    
    // Pre-populate with sells at price 100
    for (std::size_t i = 0; i < bookDepth; ++i) {
        book.addLimitOrder(Side::Sell, 100, 1, i + 1, 1);
    }
    
    uint64_t matchId = bookDepth + 1;
    uint64_t replenishId = bookDepth + 100000;
    
    for (auto _ : state) {
        // Aggressive buy matches one resting sell
        book.addLimitOrder(Side::Buy, 100, 1, matchId++, 2);
        
        benchmark::DoNotOptimize(book.bestAsk());
        benchmark::ClobberMemory();
        
        // Replenish the resting order (untimed but unavoidable for sustained benchmark)
        state.PauseTiming();
        book.addLimitOrder(Side::Sell, 100, 1, replenishId++, 1);
        state.ResumeTiming();
    }
    
    state.SetItemsProcessed(state.iterations());
}

BENCHMARK(BM_SingleMatch)
    ->Arg(10)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: PRICE LEVEL SWEEP (Match across multiple levels)
// Fixed: Pre-build book, measure only the sweep, replenish after
// ─────────────────────────────────────────────────────────────────────────────

static void BM_MultiLevelSweep(benchmark::State& state) {
    const auto numLevels = static_cast<std::size_t>(state.range(0));
    const auto sweepQty = static_cast<uint32_t>(numLevels * 10);
    
    OrderBook book(numLevels * 20 + 1000, noOpCallback);
    
    // Initial population: sells at different price levels
    uint64_t nextId = 1;
    for (std::size_t i = 0; i < numLevels; ++i) {
        book.addLimitOrder(Side::Sell, 100 + static_cast<uint32_t>(i), 10, nextId++, 1);
    }
    
    uint64_t sweepId = 1000000;
    
    for (auto _ : state) {
        // Time: aggressive buy sweeping all levels
        book.addLimitOrder(Side::Buy, 100 + static_cast<uint32_t>(numLevels), 
                          sweepQty, sweepId++, 2);
        
        benchmark::DoNotOptimize(book.bestAsk());
        benchmark::ClobberMemory();
        
        // Replenish levels (untimed)
        state.PauseTiming();
        for (std::size_t i = 0; i < numLevels; ++i) {
            book.addLimitOrder(Side::Sell, 100 + static_cast<uint32_t>(i), 10, nextId++, 1);
        }
        state.ResumeTiming();
    }
    
    state.SetItemsProcessed(state.iterations());
    state.SetLabel("levels swept");
}

BENCHMARK(BM_MultiLevelSweep)
    ->Arg(1)
    ->Arg(5)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: BEST BID/ASK ACCESS
// Measures time to access bestBid/bestAsk interleaved with book modifications
// This prevents the compiler from hoisting/eliminating the accesses
// ─────────────────────────────────────────────────────────────────────────────

static void BM_BestBidAskAccess(benchmark::State& state) {
    const auto bookDepth = static_cast<std::size_t>(state.range(0));
    
    InputGenerator gen;
    auto inputs = gen.generateRestingOrders(bookDepth);
    
    OrderBook book(bookDepth + 10000, noOpCallback);
    for (const auto& input : inputs) {
        book.addLimitOrder(input.side, input.price, input.quantity,
                          input.id, input.participantId);
    }
    
    // To prevent compiler from hoisting bestBid/bestAsk out of loop,
    // we interleave accesses with actual modifications to the book
    uint64_t id = bookDepth + 1;
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> priceDist(50, 80);
    
    for (auto _ : state) {
        // Access best bid/ask
        auto* bid = book.bestBid();
        auto* ask = book.bestAsk();
        benchmark::DoNotOptimize(bid);
        benchmark::DoNotOptimize(ask);
        
        // Modify the book (this prevents hoisting of the above)
        book.addLimitOrder(Side::Buy, priceDist(rng), 1, id, 1);
        book.cancelOrder(id);
        ++id;
        
        benchmark::ClobberMemory();
    }
    
    state.SetItemsProcessed(state.iterations() * 2);  // 2 accesses per iteration
}

BENCHMARK(BM_BestBidAskAccess)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kNanosecond);

// ─────────────────────────────────────────────────────────────────────────────
// BENCHMARK: THROUGHPUT (Orders per second)
// Fixed: Randomize prices/sides to prevent trivial prediction
// ─────────────────────────────────────────────────────────────────────────────

static void BM_Throughput_AddCancel(benchmark::State& state) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<uint32_t> priceDist(90, 110);
    std::uniform_int_distribution<uint32_t> qtyDist(1, 100);
    
    OrderBook book(100000, noOpCallback);
    uint64_t id = 0;
    
    for (auto _ : state) {
        ++id;
        uint32_t price = priceDist(rng);
        uint32_t qty = qtyDist(rng);
        Side side = (id % 2 == 0) ? Side::Buy : Side::Sell;
        
        book.addLimitOrder(side, price, qty, id, id % 100);
        book.cancelOrder(id);
        
        benchmark::ClobberMemory();
    }
    
    benchmark::DoNotOptimize(book.bestBid());
    state.SetItemsProcessed(state.iterations() * 2);  // 2 ops per iteration
}

BENCHMARK(BM_Throughput_AddCancel)->Unit(benchmark::kNanosecond);

BENCHMARK_MAIN();
