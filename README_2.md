# Limit Order Book v1

A C++20 implementation of a limit order book and matching engine modeled after
the price-time priority systems used by equities and futures exchanges. The goal
is to produce a correct, deterministic matching engine with realistic exchange
semantics, while keeping the hot path lean enough to measure and optimize.

---

## Table of Contents

1. [Overview](#overview)
2. [Architecture](#architecture)
3. [Data Structures](#data-structures)
4. [Order Types](#order-types)
5. [Public API](#public-api)
6. [Concurrency Model](#concurrency-model)
7. [Dependencies](#dependencies)
8. [Build](#build)
9. [Testing](#testing)
10. [Benchmarks](#benchmarks)
11. [Next Steps](#next-steps)

---

## Overview

A limit order book (LOB) is the core data structure of an electronic exchange.
It organizes resting buy and sell interest by price and time, and matches
incoming aggressive orders against that resting liquidity according to
**price-time priority** (FIFO within each price level).

This project implements:

- A book that maintains bids and asks sorted by price, then FIFO within each
  price level.
- A matching engine that produces `Trade` records for every fill.
- Five order types covering the most common exchange TIF variants.
- An L2-style aggregated market-depth view (`GetOrderInfos`).
- A background pruner for Good-For-Day orders driven by wall-clock time.
- A mutex-based concurrency model that protects all mutations and reads.
- A GoogleTest suite driven by plain-text scenario files.
- A micro-benchmark harness measuring Add, Cancel, Modify, and Match latency
  with per-operation nanosecond sampling.

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                         Orderbook                           │
│                                                             │
│  bids_  std::map<Price, OrderPointers, greater>             │
│  asks_  std::map<Price, OrderPointers, less>                │
│  orders_ std::unordered_map<OrderId, OrderEntry>            │
│  data_   std::unordered_map<Price, LevelData>               │
│                                                             │
│  ┌──────────────────┐   ┌──────────────────────────────┐   │
│  │   AddOrder()     │   │  PruneGoodForDayOrders()     │   │
│  │   CancelOrder()  │   │  (background thread)         │   │
│  │   ModifyOrder()  │   │  wakes at 16:00 wall clock   │   │
│  │   MatchOrders()  │   └──────────────────────────────┘   │
│  └──────────────────┘                                       │
└─────────────────────────────────────────────────────────────┘
         │ produces
         ▼
   std::vector<Trade>   (bid TradeInfo + ask TradeInfo per fill)
```

### Component files

| File | Purpose |
|---|---|
| `Orderbook.h` / `Orderbook.cpp` | Book, matching engine, GFD pruner |
| `Order.h` | Immutable order representation with fill state |
| `OrderModify.h` | Modify request (cancel-then-reinsert semantics) |
| `OrderType.h` | `enum class OrderType` |
| `Side.h` | `enum class Side { Buy, Sell }` |
| `Trade.h` / `TradeInfo.h` | Fill record returned from `AddOrder`/`ModifyOrder` |
| `LevelInfo.h` / `OrderbookLevelInfos.h` | L2 depth snapshot types |
| `Usings.h` | Type aliases — `Price`, `Quantity`, `OrderId` |
| `Constants.h` | `InvalidPrice` sentinel for market orders |
| `main.cpp` | Minimal smoke-test entry point |
| `bench/bench.cpp` | Micro-benchmark harness (standalone) |
| `test/test.cpp` | GoogleTest fixture driven by text scenario files |
| `test/TestFiles/*.txt` | Scenario scripts: actions + expected result |
| `that_harness.cpp` | Concurrent stress harness used with ThreadSanitizer |

---

## Data Structures

### Price levels — `std::map`

```cpp
std::map<Price, OrderPointers, std::greater<Price>> bids_;
std::map<Price, OrderPointers, std::less<Price>>    asks_;
```

`std::map` keeps prices in sorted order and provides O(log n) insert/erase and
O(1) access to the best price via `.begin()`. The comparators are chosen so that
`bids_.begin()` is always the highest bid and `asks_.begin()` is always the
lowest ask — no searching required to find the inside market.

### Orders at each price level — `std::list`

```cpp
using OrderPointers = std::list<OrderPointer>;
```

Each map value is a doubly-linked list of `shared_ptr<Order>`. A list is used
because:

- `push_back` is O(1) — new orders join the back (FIFO tail).
- `pop_front` is O(1) — the oldest order at a price is consumed first.
- Erase via a stored iterator is O(1) — cancellation does not require
  searching the list; the iterator is held in `OrderEntry`.

The tradeoff is poor cache locality relative to a flat array, but O(1) cancel
is considered more important for a book that supports mid-queue cancellation.

### Order lookup table — `std::unordered_map<OrderId, OrderEntry>`

```cpp
struct OrderEntry {
    OrderPointer          order_;
    OrderPointers::iterator location_;
};

std::unordered_map<OrderId, OrderEntry> orders_;
```

Maps every live order ID to its `shared_ptr` and to the list iterator that
points at it inside the per-price list. This gives O(1) average cancel,
O(1) average modify (lookup + cancel + reinsert), and a single truth source
for book size.

### Per-price aggregate cache — `std::unordered_map<Price, LevelData>`

```cpp
struct LevelData {
    Quantity quantity_;  // aggregate remaining quantity at this price
    Quantity count_;     // number of resting orders at this price
};

std::unordered_map<Price, LevelData> data_;
```

Maintained in sync with `bids_` / `asks_` via `OnOrderAdded`,
`OnOrderCancelled`, and `OnOrderMatched`. Exists entirely to support the
Fill-or-Kill pre-check (`CanFullyFill`): rather than iterating all orders on
relevant levels at submission time, the aggregated quantity is available in O(1)
per price. Entries are erased when `count_` reaches zero.

### Type aliases

```cpp
using Price    = std::int32_t;   // ticks / integer price
using Quantity = std::uint32_t;  // lots / shares
using OrderId  = std::uint64_t;  // monotonically assigned by caller
```

Deliberate integer types; no floating-point in the matching path.

---

## Order Types

| Type | Enum | Behavior |
|---|---|---|
| **Good-Till-Cancel** | `GoodTillCancel` | Rests in book until filled or explicitly cancelled. Standard limit order. |
| **Fill-and-Kill** | `FillAndKill` | If there is any immediate match, fill what crosses; cancel any unfilled remainder. Rejected entirely (no resting) if nothing crosses. Equivalent to IOC. |
| **Fill-or-Kill** | `FillOrKill` | Pre-checked before insertion: if `CanFullyFill` returns false the order is silently discarded. If it can fill, it is inserted and matched normally. |
| **Good-For-Day** | `GoodForDay` | Rests like GTC. A background thread wakes at 16:00 local wall clock and cancels all surviving GFD orders. Not exchange-calendar aware. |
| **Market** | `Market` | If the opposite book is non-empty, converted in-place to GTC at the **worst** price on the opposite side (`rbegin()`), then matched normally. Discarded if the opposite side is empty. This is a simplified stand-in — it does not walk multiple levels. |

### Matching invariants preserved

- **Price-time priority**: bids match at the highest available ask price (or
  better); time is broken by FIFO within a price.
- **Bids descending**: `bids_.begin()` is always the best (highest) bid.
- **Asks ascending**: `asks_.begin()` is always the best (lowest) ask.
- **No phantom fills**: `MatchOrders` only executes when `bidPrice >= askPrice`.
- **FOK atomicity**: the order is never partially inserted; it is either fully
  filled or not placed at all.

---

## Public API

```cpp
// Submit a new order. Returns all trades generated (empty if none).
Trades AddOrder(OrderPointer order);

// Remove a resting order by ID. No-op if the ID is unknown.
void CancelOrder(OrderId orderId);

// Cancel-and-reinsert with new price/quantity. Preserves order type.
// Returns trades if the reinserted order crosses.
Trades ModifyOrder(OrderModify order);

// Total number of live resting orders across both sides.
std::size_t Size() const;

// L2 snapshot: aggregated quantity per price, bids descending, asks ascending.
OrderbookLevelInfos GetOrderInfos() const;
```

`AddOrder` and `ModifyOrder` both return `Trades` — a `std::vector<Trade>`
where each `Trade` holds a `TradeInfo` for the bid leg and a `TradeInfo` for
the ask leg (order ID, price, filled quantity).

---

## Concurrency Model

All public methods acquire `ordersMutex_` (a `std::mutex`) before touching book
state. This makes all operations serialized and thread-safe, at the cost of
contention under concurrent callers.

The GFD pruner runs on a dedicated `std::thread` started in the constructor and
joined in the destructor:

- It sleeps on `shutdownConditionVariable_` with a timeout calculated to the
  next 16:00 local time.
- On wakeup it re-acquires the mutex, collects GFD order IDs, releases the
  lock, then acquires it again inside `CancelOrders` to batch-cancel.
- Shutdown signal is `std::atomic<bool> shutdown_` written under the mutex and
  notified via the condition variable, which avoids a race between the prune
  thread checking shutdown and the destructor signaling it.

The copy and move constructors and assignment operators are all `= delete`
because the background thread captures `this`.

A concurrent stress harness (`that_harness.cpp`) is provided for use with
`-fsanitize=thread` to verify there are no data races under concurrent
`AddOrder` / `CancelOrder` / `GetOrderInfos` traffic.

---

## Dependencies

### Runtime

No third-party runtime dependencies. The implementation uses only the C++20
standard library:

- `<map>`, `<list>`, `<unordered_map>`, `<vector>` — core data structures
- `<memory>` — `std::shared_ptr`
- `<mutex>`, `<condition_variable>`, `<atomic>`, `<thread>` — concurrency
- `<chrono>`, `<ctime>` — GFD wall-clock scheduling
- `<format>` — error messages in `Order::Fill` and `Order::ToGoodTillCancel`
- `<numeric>`, `<algorithm>` — `std::accumulate` in `GetOrderInfos`

### Build

| Tool | Version | Notes |
|---|---|---|
| `g++` | ≥ 13 recommended | C++20 required; `<format>` support needed |
| GNU Make | any | Drives all targets via `Makefile` |

### Test only

| Library | Notes |
|---|---|
| GoogleTest (`gtest`, `gtest_main`) | System-installed; linked with `-lgtest -lgtest_main -pthread` |

Install on Debian/Ubuntu:
```bash
sudo apt install libgtest-dev
```

---

## Build

All targets are defined in the `Makefile`.

```bash
# Release binary (main.cpp smoke test)
make

# Debug binary with AddressSanitizer + UndefinedBehaviorSanitizer
make debug

# ThreadSanitizer binary (on WSL2: run with setarch x86_64 -R ./tsan)
make tsan

# GoogleTest suite
make tests

# Micro-benchmark binary
make benchmark

# Remove all build artifacts
make clean
```

### Compiler flags summary

| Target | Flags |
|---|---|
| `orderbook` | `-std=c++20 -Wall -Wextra -Wpedantic -O2` |
| `debug` | `-std=c++20 -Wall -Wextra -Wpedantic -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer` |
| `tsan` | `-std=c++20 -Wall -Wextra -Wpedantic -g -O0 -fsanitize=thread -fno-omit-frame-pointer` |
| `tests` | `-std=c++20 -Wall -Wextra -Wpedantic -g -O0 -lgtest -lgtest_main -pthread` |
| `benchmark` | `-std=c++20 -Wall -Wextra -Wpedantic -O2 -pthread` |

> **WSL2 note:** ThreadSanitizer requires ASLR to be disabled in WSL2.
> Run the tsan binary with `setarch x86_64 -R ./tsan`.

---

## Testing

The test suite uses GoogleTest with a parameterized fixture
(`OrderbookTestsFixture`) that reads plain-text scenario files from
`test/TestFiles/`. Each file encodes a sequence of book operations and a single
expected result line.

### Scenario file format

```
A <side> <orderType> <price> <qty> <id>   # Add order
M <id> <side> <price> <qty>               # Modify order
C <id>                                    # Cancel order
R <totalOrders> <bidLevels> <askLevels>   # Expected result (must be last line)
```

### Test scenarios

| File | Scenario |
|---|---|
| `Match_GoodTillCancel.txt` | GTC bid crosses resting GTC ask; partial fill rests |
| `Match_FillAndKill.txt` | FAK bid fills what it can; remainder cancelled |
| `Match_FillOrKill_Hit.txt` | FOK fully fillable — executes |
| `Match_FillOrKill_Miss.txt` | FOK not fully fillable — rejected entirely |
| `Cancel_Success.txt` | Cancel removes order from book |
| `Modify_Side.txt` | Modify changes price; order loses time priority |
| `Match_Market.txt` | Market order converts and matches against resting ask |

### Running

```bash
make tests
./tests
```

---

## Benchmarks

The benchmark harness (`bench/bench.cpp`) measures four operations in isolation,
each with a 20,000-operation warm-up (to prime allocator arenas, CPU caches, and
branch predictors) followed by 200,000 measured operations with per-operation
`std::chrono::steady_clock` sampling.

### Benchmark design

| Benchmark | What is timed | Book state during measurement |
|---|---|---|
| **Add Order** | `AddOrder` — non-crossing GTC | Growing book; bids in [9000,9499], asks in [9501,10000]; no matching triggered |
| **Cancel Order** | `CancelOrder` — live order by ID | Resting orders pre-inserted; each measured op removes one |
| **Modify Order** | `ModifyOrder` — cancel + reinsert, non-crossing | One resting order per measured op; new price stays on same side band |
| **Match Order** | `AddOrder` — crossing, full fill on arrival | Sell resting at fixed price; buy crosses it; both sides consumed per op |

Orders are pre-constructed outside the timed region so allocation of the `Order`
object itself is excluded from the measurements. Price bands for Add/Cancel/Modify
are separated by a 2-tick spread (bids ≤ 9499, asks ≥ 9501) so those benchmarks
never trigger `MatchOrders`.

### Reported statistics

For each benchmark the harness reports throughput (ops/sec) and the full latency
distribution:

```
Operation: Add Order
Operations: 200000
Throughput: N ops/sec

Latency:
  p50:    N ns
  p99:    N ns
  p99.9:  N ns
  min:    N ns
  max:    N ns
  mean:   N ns
  stddev: N ns
```

### Algorithmic complexity (per operation)

| Operation | Complexity | Bottleneck |
|---|---|---|
| `AddOrder` (no match) | O(log P) | `std::map` insert at price P levels |
| `CancelOrder` | O(log P) | `std::map` erase of empty price level |
| `ModifyOrder` | O(log P) | Two map ops (cancel old + insert new price) |
| `MatchOrders` (single fill) | O(log P) | Level erasure after last order at price consumed |
| `CanFullyFill` (FOK pre-check) | O(L) | Iterates `data_` across L fillable price levels |
| `GetOrderInfos` | O(N) | Accumulates remaining quantity over all N orders |

P = number of distinct price levels, L = number of price levels that contribute
to the FOK quantity check, N = total live orders.

### Running

```bash
make benchmark
./benchmark
```

---

## Next Steps

### Correctness

- **True market sweep**: current market orders convert to a single worst-price
  GTC. A real sweep walks the opposite book level by level until the quantity
  is exhausted or the book is empty.
- **Exchange-calendar awareness**: the GFD pruner wakes at 16:00 local wall
  clock. Real exchanges define session boundaries per instrument and timezone.
- **Stop / stop-limit orders**: not in scope for v1 but a natural extension;
  requires a trigger-price index and a second pass through the matching engine
  after each fill.

### Performance

- **Lock contention**: the single `std::mutex` serializes all readers and
  writers. A read-write lock (`std::shared_mutex`) would allow concurrent
  `GetOrderInfos` / `Size` calls without blocking each other.
- **Allocator pressure**: every `AddOrder` call allocates a `shared_ptr`
  control block and an `Order` on the heap. A pool allocator (e.g., a slab
  per `Order` type) would reduce allocation latency and fragmentation on the
  hot path.
- **`std::list` cache locality**: linked-list nodes are scattered on the heap.
  For books where most orders rest and cancel at the inside price, a small
  ring-buffer or flat deque per level would improve cache behavior.
- **`shared_ptr` overhead**: reference-count atomics add cost on every copy of
  an `OrderPointer`. Replacing with raw pointers into a pool (with explicit
  ownership through the book) would eliminate that overhead.
- **False sharing**: `LevelData` fields (`quantity_`, `count_`) are updated
  together; padding to a cache line is only beneficial if they are accessed from
  separate threads, which the current mutex model prevents.

### Observability

- **Trade callbacks / event bus**: currently `AddOrder` returns trades as a
  value. A callback or observer pattern would allow downstream consumers
  (risk, PnL, market-data feed) to react without polling.
- **Order acknowledgement events**: `OnOrderAdded`, `OnOrderCancelled` are
  private; exposing them as hooks would support exchange-style execution reports.
- **Richer L2/L3 API**: `GetOrderInfos` aggregates quantity per level (L2).
  An L3 view exposing individual order IDs and queue positions would support
  queue-position analytics.

### Infrastructure

- **CMake**: the current `Makefile` is simple but not portable. A CMake build
  would integrate with IDEs and CI pipelines more cleanly.
- **CI / sanitizer gates**: running `debug` (ASan + UBSan) and `tsan` targets
  in CI on every push would catch regressions before they accumulate.
- **Fuzzing**: `libFuzzer` or AFL++ targeting `AddOrder` / `CancelOrder` /
  `ModifyOrder` with random input streams would stress-test invariant
  preservation beyond what scenario files cover.
