# Limit Order Book

A C++ implementation of a limit order book and matching engine,
modeled after the price-time priority systems used by major exchanges.

## Status

In progress. Core matching engine, **aggregated level data**, and **mutex-protected** add/cancel/modify/size APIs are in place.

**Implemented in code:** GTC, Fill-and-Kill (IOC-style remainder cancel), Fill-or-Kill (reject-before-rest if not fully fillable), Market orders (converted to GTC at the **worst** price on the opposite book when that side is non-empty), and a **Good-For-Day background pruner** (cancels GFD orders on a wall-clock schedule).

**Still rough / planned:** GoogleTest suite, concurrency hardening (e.g. snapshot APIs under the same lock as mutations), and richer market semantics (true sweep against multiple levels rather than single-price conversion).

## Design

- Price levels stored in `std::map` (O(log n) insert, ordered iteration for best bid/ask via `.begin()`).
- Orders at each price level in `std::list` for O(1) FIFO insert/erase while preserving time priority.
- O(1) order lookup by ID via `std::unordered_map<OrderId, OrderEntry>` where `OrderEntry` holds a list iterator for O(1) cancellation.
- `std::unordered_map<Price, LevelData> data_` tracks per-price aggregate quantity (and order count) to support FOK checks and level updates.
- `AddOrder` / `ModifyOrder` return `Trades` for all executions caused by the operation; `GetOrderInfos()` exposes an L2-style aggregated bid/ask view (`LevelInfo` per price).
- **Concurrency:** `std::mutex` guards the book on the hot path; a dedicated thread runs `PruneGoodForDayOrders`, stopped via `std::atomic<bool>`, `std::condition_variable`, and `join()` in the destructor.

## Order types (implementation checklist)

- [x] Good-Till-Cancel
- [x] Fill-and-Kill (IOC): Immediate-Or-Cancel - no rest if no immediate match; resting remainder on the best queue canceled after match pass
- [x] Fill-or-Kill: order rejected entirely if not fully fillable at submission
- [x] Good-For-Day: canceled by background thread (see `PruneGoodForDayOrders` in `Orderbook.cpp`)
- [x] Market: if the opposite book is non-empty, converted in-place to GTC at the **worst** opposite price, then matched like a limit

## Build

```bash
make
./orderbook
```

Produces the `orderbook` binary from `main.cpp` and `Orderbook.cpp` (C++20, `-O2`).

## Roadmap

- Unit tests (GoogleTest) for match, cancel, FOK/FAK, and modify.
- Thread-safe read paths (`GetOrderInfos`, optional depth API) and internal cancel paths that avoid re-locking the mutex during `MatchOrders`.
- Optional: true market sweep, session calendar / exchange timezone, stop / stop-limit (not in scope today).
