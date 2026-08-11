# **Order Matching Engine**

A high-performance limit order book (LOB) and matching engine implemented in modern C++.

This project demonstrates systems-level thinking, low-latency architecture, and memory-safe, allocation-free data structures inspired by real-world electronic trading engines.

---

## **Overview**

The engine maintains a central limit order book with strict **price–time priority**, supports adding and cancelling orders, and performs matching between aggressive and resting orders.

It is designed to be:

* **Deterministic**

* **Cache-efficient**

* **Allocation-free in the hot path**

* **Benchmarkable and debuggable**

The implementation uses intrusive linked lists, preallocated memory pools, sorted price levels, and a predictable control-flow path suitable for low-latency workloads.

---

## **Features**

### **Core Functionality**

* Add limit orders
* Cancel resting orders
* Automatic matching when incoming orders cross the spread
* Partial and full fills
* Self-match prevention (SMP) via participant ID
* Trade event generation via callback interface

### **Data Structure Highlights**

* Preallocated fixed-size `OrderPool`
* No dynamic allocations during matching
* Price levels stored in sorted vectors
* Intrusive FIFO queues for price-time priority
* O(1) cancel via order ID index
* Deterministic sequence-based ordering

### **Performance-Oriented Design**

* Hot code paths free of heap allocation
* Compact `Order` layout for cache locality
* `lower_bound` search for price-level lookup
* Google Benchmark for throughput measurement
* Custom latency harness for percentile distribution

---

## **Performance**

Measured on Apple M3 Pro (Release build, `-O3 -DNDEBUG -march=native`):

| Operation | Latency (p50) | Throughput |
|-----------|---------------|------------|
| Add Resting Order | 50 ns | 20-22 M/s |
| Add Crossing Order | 60 ns | 16-18 M/s |
| Cancel Order | 67 ns | 16-23 M/s |
| 10-Level Sweep | 250 ns | ~4 M/s |
| Best Bid/Ask | ~2-3 ns | O(1) |

**Key properties:**
- Crossing orders that fully fill allocate **0 heap memory**
- Resting orders allocate 1 `unordered_map` node
- Tight tail latencies: p99 typically < 2x p50

See [docs/perf-notes.md](docs/perf-notes.md) for full methodology, percentiles, and reproducibility notes.

---

## **Architecture**

### **Order Representation**

Each order is represented as an intrusive node in a doubly-linked list:

```cpp
struct Order {
    uint64_t orderId;
    uint32_t price;
    uint32_t quantity;
    uint64_t sequence;
    Side side;
    uint64_t participantId;
    Order* next;
    Order* prev;
};
```

### **Price Levels**

A price level maintains a FIFO queue of orders:

```cpp
struct PriceLevel {
    uint32_t price;
    uint32_t totalQuantity;
    Order* head;
    Order* tail;
    void addToTail(Order* o);
    void remove(Order* o);
};
```

### **Order Pool**

A fixed-size contiguous pool ensures predictable memory behavior:

```cpp
OrderPool pool(maxOrders);
// allocate() and deallocate() operate via a free list
```

### **Order Book**

The `OrderBook` is a template class that maintains:

* sorted vectors of bid and ask price levels
* `unordered_map` index for O(1) order lookup (pre-reserved)
* sequence counter for price-time priority
* a trade callback interface (templated to avoid `std::function` overhead)

---

## **API Summary**

```cpp
template<typename TradeCallback>
class OrderBook {
public:
    OrderBook(std::size_t capacity, TradeCallback callback);
    
    void addLimitOrder(Side side, uint32_t price, uint32_t quantity, 
                       uint64_t orderId, uint64_t participantId);
    void cancelOrder(uint64_t orderId);
    
    const PriceLevel* bestBid() const;
    const PriceLevel* bestAsk() const;
};
```

Trade event callback:

```cpp
struct Trade {
    uint64_t incomingOrderId;
    uint64_t restingOrderId;
    uint32_t price;
    uint32_t quantity;
};

// Example usage:
OrderBook book(10000, [](const Trade& t) {
    std::cout << "Trade: " << t.quantity << " @ " << t.price << "\n";
});
```

---

## **Matching Logic**

### **BUY incoming**

Matches against lowest ask levels:

```
while incoming.price >= bestAsk.price and qty > 0:
    if incoming.participantId == resting.participantId:
        cancel incoming (self-match prevention)
    fill against head of bestAsk FIFO
```

### **SELL incoming**

Matches against highest bid levels:

```
while incoming.price <= bestBid.price and qty > 0:
    if incoming.participantId == resting.participantId:
        cancel incoming (self-match prevention)
    fill against head of bestBid FIFO
```

Unfilled quantity becomes a resting order.

---

## **Invariants**

The engine maintains strict invariants:

### **Order Invariants**

* FIFO order preserved via `sequence`
* Linked lists contain no cycles
* `quantity > 0` for all resting orders

### **Price Level Invariants**

* Sorted: bids ascending, asks descending (best at back)
* `totalQuantity` matches the sum of quantities in the list
* No empty levels remain stored

### **Order Book Invariants**

* `orderIndex[id]` is correct or absent
* `bestBid()` and `bestAsk()` return pointer to back of sorted vectors
* Only resting orders (quantity > 0) are indexed

These invariants are verified in tests and debug builds.

---

## **Project Structure**

```
.
├── include/
│   ├── order_book.h        # Header-only OrderBook template
│   ├── order_pool.h
│   ├── price_level.h
│   └── types.h
├── src/
│   ├── order_pool.cpp
│   ├── price_level.cpp
│   └── main.cpp
├── tests/
│   ├── order_book_matching_tests.cpp
│   ├── order_book_cancel_test.cpp
│   ├── order_book_smp_tests.cpp
│   └── allocation_test.cpp
├── benchmarks/
│   ├── order_book_bench.cpp      # Google Benchmark suite
│   └── latency_percentiles.cpp   # Custom latency harness
├── docs/
│   └── perf-notes.md
└── CMakeLists.txt
```

---

## **Benchmarking**

### **Throughput (Google Benchmark)**

```bash
./build-release/order_book_bench
```

Measures ops/sec for add, cancel, match, and mixed workloads.

### **Latency Percentiles (Custom Harness)**

```bash
./build-release/latency_percentiles
```

Measures p50/p90/p99/p99.9 for individual operations using `mach_absolute_time()` (macOS) with batching to overcome timer resolution.

### **Building Benchmarks**

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-release
```

---

## **Building & Running**

### **Build (Debug)**

```bash
cmake -B build
cmake --build build
```

### **Build (Release with Benchmarks)**

```bash
cmake -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_BENCHMARKS=ON
cmake --build build-release
```

### **Run Tests**

```bash
cd build && ctest --output-on-failure
```

### **Run Benchmarks**

```bash
./build-release/order_book_bench
./build-release/latency_percentiles
```

---

## **Future Improvements**

* Lock-free or wait-free matching engine variant
* NUMA-aware memory placement
* Market/IOC/FOK order types
* Instrumentation hooks (ETW, LTTng)
* FPGA or DPDK-style order ingestion layer
* Multi-symbol support
* `perf stat` measurements on Linux (cycles, IPC, cache misses)
