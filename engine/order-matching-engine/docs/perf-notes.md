# Performance Profiling Report

**Date:** 2025-01-03  

---

## Environment

| Property | Value |
|----------|-------|
| CPU | Apple M3 Pro (6P + 6E cores) |
| Memory | 36 GB |
| OS | macOS 15.6.1 (Sequoia) |
| Compiler | Apple Clang 17.0.0 |
| C++ Standard | C++20 |
| Build | Release (-O3 -DNDEBUG -march=native) |

---

## Measurement Methodology

### Two Tools, Two Purposes

| Tool | Use For | Notes |
|------|---------|-------|
| **Google Benchmark** | Throughput (ops/sec) | Big batches, amortized cost |
| **Latency Harness** | Per-op percentiles | Direct timing, batched where needed |

### Timer Characteristics
- Apple Silicon `mach_absolute_time()`: ~41 ns resolution
- Operations < 41 ns require batching (time N ops, divide by N)
- All benchmarks use `asm volatile` / `DoNotOptimize` to prevent dead-code elimination

### Google Benchmark Caveat
- `PauseTiming()`/`ResumeTiming()` adds ~400-500 ns overhead per call
- GB is used for throughput only; latency distribution comes from custom harness

---

## Executive Summary

| Operation | Latency (p50) | Throughput | Notes |
|-----------|---------------|------------|-------|
| Add Resting Order | 50 ns | 20-22 M/s | Full API call |
| Add Crossing Order | 60 ns | 16-18 M/s | Full API including match |
| Cancel Order | 67 ns | 16-23 M/s | Stable-state book |
| 10-Level Sweep | 250 ns | ~4 M/s | Single order sweeps 10 levels |
| Best Bid/Ask | ~2-3 ns | - | O(1), vector.back() access |

*Throughput varies with batch size and book state; ranges reflect observed measurements.*

---

## Throughput Benchmarks (Google Benchmark)

### Add Orders (Non-Crossing, Resting)

| Batch Size | Time | Per-Op | Throughput |
|------------|------|--------|------------|
| 100 | 5.09 µs | 51 ns | 19.6 M/s |
| 1,000 | 46.3 µs | 46 ns | 21.6 M/s |
| 10,000 | 455 µs | 46 ns | **22.0 M/s** |

### Match Heavy (Crossing Orders)

| Batch Size | Time | Per-Op | Throughput |
|------------|------|--------|------------|
| 100 | 3.14 µs | 63 ns | 15.9 M/s |
| 1,000 | 27.3 µs | 55 ns | 18.3 M/s |
| 10,000 | 276 µs | 55 ns | **18.1 M/s** |

### Cancel Orders

| Batch Size | Time | Per-Op | Throughput |
|------------|------|--------|------------|
| 100 | 4.52 µs | 45 ns | 22.1 M/s |
| 1,000 | 43.8 µs | 44 ns | **22.8 M/s** |
| 10,000 | 619 µs | 62 ns | 16.2 M/s |

### Mixed Workload (70% add, 20% cancel, 10% match)

| Batch Size | Time | Per-Op | Throughput |
|------------|------|--------|------------|
| 1,000 | 36.1 µs | 36 ns | **27.7 M/s** |
| 10,000 | 405 µs | 41 ns | 24.7 M/s |
| 100,000 | 4.57 ms | 46 ns | 21.9 M/s |

### Multi-Level Sweep

| Levels | Time | Per-Level Cost |
|--------|------|----------------|
| 1 | 514 ns | baseline |
| 5 | 611 ns | ~24 ns/level |
| 10 | 754 ns | ~24 ns/level |
| 50 | 1,804 ns | ~26 ns/level |
| 100 | 3,139 ns | ~26 ns/level |

---

## Latency Percentiles (Custom Harness)

### Add Resting Order (batched 100 ops)

Full `addLimitOrder()` API call for a non-crossing order.

| Percentile | Latency |
|------------|---------|
| Min | 40 ns |
| **p50** | **50 ns** |
| p90 | 67 ns |
| p99 | 87 ns |
| p99.9 | 140 ns |
| Max | 218 ns |

### Add Crossing Order [Full API] (batched 100 ops)

Full `addLimitOrder()` API call for a crossing order, including:
- Price level lookup
- Order matching (1:1 fill)
- Trade callback invocation  
- Resting order removal
- Hash index operations

| Percentile | Latency |
|------------|---------|
| Min | 27 ns |
| **p50** | **60 ns** |
| p90 | 72 ns |
| p99 | 94 ns |
| p99.9 | 170 ns |
| Max | 2,719 ns |

### Cancel Order [Stable-State] (batched 100 ops)

Full `cancelOrder()` API call on a stable-size book (~10k orders).
Book is replenished between batches to avoid draining artifacts.

| Percentile | Latency |
|------------|---------|
| Min | 47 ns |
| **p50** | **67 ns** |
| p90 | 91 ns |
| p99 | 149 ns |
| p99.9 | 215 ns |
| Max | 285 ns |

### Multi-Level Sweep (10 levels)

Single aggressive order that sweeps 10 price levels.

| Percentile | Latency |
|------------|---------|
| Min | 208 ns |
| **p50** | **250 ns** |
| p90 | 333 ns |
| p99 | 375 ns |
| Max | 666 ns |

### Multi-Level Sweep (50 levels)

| Percentile | Latency |
|------------|---------|
| Min | 1,333 ns |
| **p50** | **1,375 ns** |
| p90 | 1,708 ns |
| p99 | 1,833 ns |

### Best Bid/Ask Access (batched 1000 pairs)

O(1) access to `bids_.back()` / `asks_.back()`.
Batched to overcome timer resolution; ~2-3 ns is plausible for inlined vector access.

| Percentile | Latency |
|------------|---------|
| Min | 2 ns |
| **p50** | **2 ns** |
| p90 | 2 ns |
| p99 | 3 ns |
| Max | 11 ns |

*Note: Sub-5ns measurements are at the limit of timing precision. The key property is O(1) constant-time access regardless of book size.*

---

## Mixed Workload Breakdown

Per-operation latency from a realistic mixed workload (70/20/10 split).

| Operation | Samples | p50 | p99 |
|-----------|---------|-----|-----|
| Add (Resting) | 7,022 | 41 ns | 83 ns |
| Cancel | 1,985 | 41 ns | 83 ns |
| Add (Crossing) | 993 | 41 ns | 166 ns |

---

## Allocation Profile

| Operation | Heap Allocations | Notes |
|-----------|------------------|-------|
| Add order (resting) | 1 | `unordered_map` node for order index |
| Add order (crossing, fully filled) | 0 | Not indexed; uses pool memory only |
| Add order (crossing, partial fill) | 1 | Remainder rests, gets indexed |
| Cancel order | 0 | Index erase, pool dealloc (no heap) |
| Match (resting side) | 0 | Index erase, pool dealloc (no heap) |
| Best bid/ask | 0 | O(1) vector access |

**Design**: Only orders that rest in the book are inserted into `orderIndex_`.
Crossing orders that fully match are allocated from the pre-sized `OrderPool`,
matched, and returned to the pool without ever touching `orderIndex_`.

---

## Key Takeaways

1. **~50-60 ns per add** (resting or crossing, full API)
2. **~67 ns per cancel** (stable-state book)
3. **~25 ns per price level swept**
4. **Best bid/ask is O(1)** (effectively `vector.back()`; measured ~2-3 ns batched)
5. **16-28 M ops/sec throughput** (varies with batch size and book state)
6. **Tight tail latencies**: p99 typically < 2x p50
7. **Crossing orders that fully fill allocate 0 heap memory**

---

## Reproducing Results

```bash
# Build release
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-release

# Throughput benchmarks (Google Benchmark)
./build-release/order_book_bench

# Latency percentiles (custom harness)
./build-release/latency_percentiles

# With more samples
./build-release/latency_percentiles 50000
```

---

## Future Work

- [ ] Add `perf stat` measurements on Linux (cycles, IPC, cache misses)
- [ ] Consider `rdtsc`/`rdtscp` for sub-nanosecond timing on x86
- [ ] Profile with Intel VTune or Apple Instruments
- [ ] Larger book sizes (100k+ orders)
