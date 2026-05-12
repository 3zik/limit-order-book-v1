# Limit Order Book

A C++ implementation of a limit order book and matching engine,
modeled after the price-time priority systems used by major exchanges.

## Status
In progress. Core matching engine and GTC/FAK orders implemented.
Working on: FOK, GFD, Market orders, GoogleTest suite.

## Design
- Price levels stored in `std::map` (O(log n) insert, ordered iteration
  for best bid/ask via `.begin()`)
- Orders at each price level in `std::list` for O(1) FIFO insert/erase
  while preserving time priority
- O(1) order lookup by ID via `std::unordered_map<OrderId, OrderEntry>`
  where OrderEntry holds a list iterator for O(1) cancellation

## Order Types
- [x] Good-Till-Cancel
- [x] Fill-and-Kill (IOC)
- [ ] Fill-or-Kill
- [ ] Good-For-Day
- [ ] Market

## Build
`make`

## Roadmap
...
