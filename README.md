# Limit Order Book v1

A C++20 limit order book and matching engine modeled on the price-time priority systems used by equities and futures exchanges. The aim is a correct, deterministic engine with realistic exchange semantics, kept lean enough on the hot path to measure and optimize. This is v1; a latency-focused v2 is in progress.

## Overview

A limit order book organizes resting buy and sell interest by price and time, then matches incoming aggressive orders against that liquidity under price-time priority (FIFO within each price level).

This implementation provides:

- Bids and asks sorted by price, FIFO within each level.
- A matching engine that emits a `Trade` record for every fill.
- Five order types covering the common TIF variants.
- An L2 aggregated depth view (`GetOrderInfos`).
- A background pruner for Good-For-Day orders driven by wall-clock time.
- A single-mutex concurrency model guarding mutations and reads.
- A GoogleTest suite driven by plain-text scenario files.
- A micro-benchmark harness with per-operation nanosecond sampling.

## Layout

| File | Purpose |
|---|---|
| `Orderbook.h` / `.cpp` | Book, matching engine, GFD pruner |
| `Order.h` | Order representation with fill state |
| `OrderModify.h` | Modify request (cancel then reinsert) |
| `OrderType.h`, `Side.h` | Enums |
| `Trade.h` / `TradeInfo.h` | Fill records returned by `AddOrder` / `ModifyOrder` |
| `LevelInfo.h` / `OrderbookLevelInfos.h` | L2 snapshot types |
| `Usings.h`, `Constants.h` | Type aliases and the `InvalidPrice` sentinel |
| `main.cpp` | Smoke-test entry point |
| `bench/bench.cpp` | Benchmark harness |
| `test/test.cpp`, `test/TestFiles/*.txt` | GoogleTest fixture and scenario scripts |
| `that_harness.cpp` | Concurrent stress harness for ThreadSanitizer |

## Data structures

Price levels live in two `std::map`s, one per side:

```cpp
std::map<Price, OrderPointers, std::greater<Price>> bids_;
std::map<Price, OrderPointers, std::less<Price>>    asks_;
```

The comparators make `bids_.begin()` the highest bid and `asks_.begin()` the lowest ask, so the inside market is O(1) to reach, with O(log n) insert and erase.

Orders at each level sit in a `std::list` of `shared_ptr<Order>`:

```cpp
using OrderPointers = std::list<OrderPointer>;
```

`push_back` (FIFO tail) and `pop_front` (oldest first) are O(1), and erase by a stored iterator is O(1), so a mid-queue cancel never scans the list. The cost is weaker cache locality than a flat array, a tradeoff revisited in v2.

O(1) lookup by ID:

```cpp
struct OrderEntry {
    OrderPointer            order_;
    OrderPointers::iterator location_;
};
std::unordered_map<OrderId, OrderEntry> orders_;
```

The stored iterator gives O(1) average cancel and modify, and one source of truth for book size.

Per-price aggregate cache:

```cpp
struct LevelData { Quantity quantity_; Quantity count_; };
std::unordered_map<Price, LevelData> data_;
```

Kept in sync through `OnOrderAdded` / `OnOrderCancelled` / `OnOrderMatched`. It exists so the Fill-or-Kill pre-check (`CanFullyFill`) can read aggregated quantity per price in O(1) instead of walking every order. Entries are dropped when `count_` reaches zero.

Prices and quantities are integer types (`Price = int32_t`, `Quantity = uint32_t`, `OrderId = uint64_t`); there is no floating point in the matching path.

## Order types

| Type | Behavior |
|---|---|
| Good-Till-Cancel | Rests until filled or cancelled. Standard limit. |
| Fill-and-Kill (IOC) | Fills whatever crosses on arrival, cancels the rest, never rests. |
| Fill-or-Kill | Pre-checked with `CanFullyFill`; placed and matched only if it can fully fill, otherwise discarded. Never partially inserted. |
| Good-For-Day | Rests like GTC; the background thread cancels survivors at 16:00 local time. Not exchange-calendar aware. |
| Market | If the opposite side is non-empty, converted in place to a GTC at the worst opposite price (`rbegin()`) and matched. Discarded against an empty book. Single-level only; a true sweep is planned. |

Matching holds price-time priority, executes only when `bidPrice >= askPrice`, and keeps FOK atomic: an FOK order is either fully filled or never placed.

## Public API

```cpp
Trades              AddOrder(OrderPointer order);   // trades generated, empty if none
void                CancelOrder(OrderId orderId);   // no-op on unknown ID
Trades              ModifyOrder(OrderModify order); // cancel + reinsert, keeps type
std::size_t         Size() const;                   // live resting orders
OrderbookLevelInfos GetOrderInfos() const;          // L2 snapshot, bids desc / asks asc
```

Each `Trade` carries a `TradeInfo` for the bid leg and one for the ask leg (order ID, price, filled quantity).

## Concurrency

A single `std::mutex` serializes every public operation, so mutations and the snapshot reads run one at a time. The GFD pruner runs on a dedicated thread started in the constructor and joined in the destructor: it waits on a `std::condition_variable` with a timeout to the next 16:00, then re-locks to collect and cancel surviving GFD orders. The `std::atomic<bool>` shutdown flag is written under the mutex before notifying. Copy and move are deleted because the thread captures `this`.

`that_harness.cpp` drives concurrent `AddOrder` / `CancelOrder` / `GetOrderInfos` traffic under ThreadSanitizer to check the locking. Broader read concurrency (a `std::shared_mutex`) is a v2 item.

## Build

```bash
make            # release binary (main.cpp), -O2
make debug      # ASan + UBSan
make tsan       # ThreadSanitizer
make tests      # GoogleTest suite
make benchmark  # micro-benchmark
make clean
```

All targets build with `-std=c++20 -Wall -Wextra -Wpedantic`. g++ 13 or newer is needed for `<format>`. The test target links system GoogleTest (`sudo apt install libgtest-dev`, then `-lgtest -lgtest_main -pthread`).

WSL2 note: ThreadSanitizer needs ASLR off, so run the tsan binary as `setarch x86_64 -R ./tsan`.

## Testing

The GoogleTest fixture (`OrderbookTestsFixture`) reads scenario files from `test/TestFiles/`. Each file is a sequence of operations plus one expected-result line:

```
A <side> <orderType> <price> <qty> <id>   # add
M <id> <side> <price> <qty>               # modify
C <id>                                    # cancel
R <totalOrders> <bidLevels> <askLevels>   # expected result, last line
```

Scenarios cover GTC matching, FAK remainder cancel, FOK hit and miss, cancel, modify (a price change drops time priority), and market conversion.

```bash
make tests && ./tests
```

## Benchmarks

`bench/bench.cpp` times Add, Cancel, Modify, and Match in isolation. Each runs a 20,000-operation warmup (to prime allocator arenas, caches, and branch predictors), then 200,000 measured operations sampled per call with `steady_clock`. Order objects are built outside the timed region so their allocation is excluded, and the Add/Cancel/Modify price bands are kept two ticks apart (bids <= 9499, asks >= 9501) so those paths never trigger matching. Each benchmark reports throughput and the latency distribution (p50, p99, p99.9, min, max, mean, stddev).

Per-operation complexity, with P distinct price levels, L fillable levels, N live orders:

| Operation | Complexity | Bottleneck |
|---|---|---|
| `AddOrder` (no match) | O(log P) | map insert |
| `CancelOrder` | O(log P) | map erase of an emptied level |
| `ModifyOrder` | O(log P) | cancel old level + insert new |
| `MatchOrders` (single fill) | O(log P) | level erase after last order consumed |
| `CanFullyFill` (FOK) | O(L) | scan of `data_` across fillable levels |
| `GetOrderInfos` | O(N) | accumulate remaining quantity |

```bash
make benchmark && ./benchmark
```

## Roadmap

Finishing v1: broaden the GoogleTest coverage and run the `debug` and `tsan` targets as a gate before each commit.

v2 (latency) is the main thread of work. Replace `shared_ptr` ownership with a pool allocator to cut allocation cost and reference-count atomics on the hot path; replace the per-level `std::list` with cache-friendlier storage (flat price ladder, intrusive list); and add a `std::shared_mutex` so `GetOrderInfos` and `Size` can read concurrently.

Later extensions: a true multi-level market sweep, exchange-calendar session boundaries, stop and stop-limit orders, an L3 view exposing per-order queue positions, and trade or ack callbacks for downstream consumers.
