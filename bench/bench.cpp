#include "../Orderbook.cpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

namespace BenchConfig
{
    constexpr std::uint32_t kSeed = 42;

    constexpr std::size_t kWarmupOps   = 20'000;
    constexpr std::size_t kMeasuredOps = 200'000;

    // Disjoint price bands so Add/Cancel/Modify benchmarks never cross and
    // trigger matching -- that path is isolated in the matching benchmark.
    constexpr Price kBidLow  = 9'000;
    constexpr Price kBidHigh = 9'499;
    constexpr Price kAskLow  = 9'501;
    constexpr Price kAskHigh = 10'000;

    // Fixed price used to force a full-fill match on every measured op in
    // the matching benchmark.
    constexpr Price kMatchPrice = 10'000;

    constexpr Quantity kQtyLow  = 1;
    constexpr Quantity kQtyHigh = 100;
}

// ---------------------------------------------------------------------------
// Latency statistics
// ---------------------------------------------------------------------------

struct LatencyStats
{
    std::int64_t minNs  = 0;
    std::int64_t maxNs  = 0;
    std::int64_t p50Ns  = 0;
    std::int64_t p99Ns  = 0;
    std::int64_t p999Ns = 0;
    double meanNs       = 0.0;
    double stddevNs      = 0.0;

    static LatencyStats Compute(std::vector<std::int64_t> samples)
    {
        LatencyStats stats;
        if (samples.empty())
            return stats;

        std::sort(samples.begin(), samples.end());

        const std::size_t n = samples.size();
        stats.minNs = samples.front();
        stats.maxNs = samples.back();

        const double sum = std::accumulate(samples.begin(), samples.end(), 0.0);
        stats.meanNs = sum / static_cast<double>(n);

        double sqDiffSum = 0.0;
        for (const auto sample : samples)
        {
            const double diff = static_cast<double>(sample) - stats.meanNs;
            sqDiffSum += diff * diff;
        }
        stats.stddevNs = std::sqrt(sqDiffSum / static_cast<double>(n));

        auto percentile = [&](double p) -> std::int64_t
        {
            const auto rank = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n))) - 1;
            const auto index = std::min(rank, n - 1);
            return samples[index];
        };

        stats.p50Ns  = percentile(0.50);
        stats.p99Ns  = percentile(0.99);
        stats.p999Ns = percentile(0.999);

        return stats;
    }
};

struct BenchResult
{
    std::string name;
    std::size_t opCount = 0;
    double elapsedSeconds = 0.0;
    LatencyStats latency;
};

void PrintResult(const BenchResult& result)
{
    const double opsPerSec = result.opCount / result.elapsedSeconds;

    std::cout << "Operation: " << result.name << '\n';
    std::cout << "Operations: " << result.opCount << '\n';
    std::cout << "Throughput: " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec\n";
    std::cout << '\n';
    std::cout << "Latency:\n";
    std::cout << "  p50:    " << result.latency.p50Ns << " ns\n";
    std::cout << "  p99:    " << result.latency.p99Ns << " ns\n";
    std::cout << "  p99.9:  " << result.latency.p999Ns << " ns\n";
    std::cout << "  min:    " << result.latency.minNs << " ns\n";
    std::cout << "  max:    " << result.latency.maxNs << " ns\n";
    std::cout << "  mean:   " << std::fixed << std::setprecision(1) << result.latency.meanNs << " ns\n";
    std::cout << "  stddev: " << std::fixed << std::setprecision(1) << result.latency.stddevNs << " ns\n";
    std::cout << "----------------------------------------\n";
}

// ---------------------------------------------------------------------------
// Order generation helpers (kept outside of timed sections)
// ---------------------------------------------------------------------------

OrderPointer MakeNonCrossingOrder(OrderId id, std::mt19937& rng,
                                   std::uniform_int_distribution<int>& sideDist,
                                   std::uniform_int_distribution<Quantity>& qtyDist)
{
    using namespace BenchConfig;

    const bool isBuy = sideDist(rng) == 0;
    const Side side = isBuy ? Side::Buy : Side::Sell;
    std::uniform_int_distribution<Price> priceDist(isBuy ? kBidLow : kAskLow, isBuy ? kBidHigh : kAskHigh);
    const Price price = priceDist(rng);
    const Quantity quantity = qtyDist(rng);

    return std::make_shared<Order>(OrderType::GoodTillCancel, id, side, price, quantity);
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------

BenchResult BenchAddOrder()
{
    using namespace BenchConfig;

    Orderbook orderbook;
    std::mt19937 rng{ kSeed };
    std::uniform_int_distribution<int> sideDist{ 0, 1 };
    std::uniform_int_distribution<Quantity> qtyDist{ kQtyLow, kQtyHigh };

    OrderId nextId = 1;

    // Warm-up: not measured, but exercises the same code paths so caches,
    // allocator arenas, and branch predictors are primed before sampling.
    for (std::size_t i = 0; i < kWarmupOps; ++i)
    {
        auto order = MakeNonCrossingOrder(nextId++, rng, sideDist, qtyDist);
        orderbook.AddOrder(order);
    }

    // Pre-build every order ahead of time so construction/allocation of the
    // Order object itself is not included in the timed region.
    std::vector<OrderPointer> orders;
    orders.reserve(kMeasuredOps);
    for (std::size_t i = 0; i < kMeasuredOps; ++i)
        orders.push_back(MakeNonCrossingOrder(nextId++, rng, sideDist, qtyDist));

    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(kMeasuredOps);

    const auto batchStart = Clock::now();
    for (auto& order : orders)
    {
        const auto opStart = Clock::now();
        orderbook.AddOrder(order);
        const auto opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    const auto batchEnd = Clock::now();

    BenchResult result;
    result.name = "Add Order";
    result.opCount = kMeasuredOps;
    result.elapsedSeconds = std::chrono::duration<double>(batchEnd - batchStart).count();
    result.latency = LatencyStats::Compute(std::move(latenciesNs));
    return result;
}

BenchResult BenchCancelOrder()
{
    using namespace BenchConfig;

    Orderbook orderbook;
    std::mt19937 rng{ kSeed };
    std::uniform_int_distribution<int> sideDist{ 0, 1 };
    std::uniform_int_distribution<Quantity> qtyDist{ kQtyLow, kQtyHigh };

    OrderId nextId = 1;

    // Warm-up: insert and cancel a disjoint batch of orders first.
    std::vector<OrderId> warmupIds;
    warmupIds.reserve(kWarmupOps);
    for (std::size_t i = 0; i < kWarmupOps; ++i)
    {
        const OrderId id = nextId++;
        orderbook.AddOrder(MakeNonCrossingOrder(id, rng, sideDist, qtyDist));
        warmupIds.push_back(id);
    }
    for (const auto id : warmupIds)
        orderbook.CancelOrder(id);

    // Resting orders to be cancelled in the measured phase.
    std::vector<OrderId> measuredIds;
    measuredIds.reserve(kMeasuredOps);
    for (std::size_t i = 0; i < kMeasuredOps; ++i)
    {
        const OrderId id = nextId++;
        orderbook.AddOrder(MakeNonCrossingOrder(id, rng, sideDist, qtyDist));
        measuredIds.push_back(id);
    }

    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(kMeasuredOps);

    const auto batchStart = Clock::now();
    for (const auto id : measuredIds)
    {
        const auto opStart = Clock::now();
        orderbook.CancelOrder(id);
        const auto opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    const auto batchEnd = Clock::now();

    BenchResult result;
    result.name = "Cancel Order";
    result.opCount = kMeasuredOps;
    result.elapsedSeconds = std::chrono::duration<double>(batchEnd - batchStart).count();
    result.latency = LatencyStats::Compute(std::move(latenciesNs));
    return result;
}

BenchResult BenchModifyOrder()
{
    using namespace BenchConfig;

    Orderbook orderbook;
    std::mt19937 rng{ kSeed };
    std::uniform_int_distribution<int> sideDist{ 0, 1 };
    std::uniform_int_distribution<Quantity> qtyDist{ kQtyLow, kQtyHigh };

    OrderId nextId = 1;

    // Warm-up: insert and modify a disjoint batch first.
    for (std::size_t i = 0; i < kWarmupOps; ++i)
    {
        const OrderId id = nextId++;
        auto order = MakeNonCrossingOrder(id, rng, sideDist, qtyDist);
        const Side side = order->GetSide();
        const bool isBuy = side == Side::Buy;
        orderbook.AddOrder(std::move(order));

        std::uniform_int_distribution<Price> priceDist(isBuy ? kBidLow : kAskLow, isBuy ? kBidHigh : kAskHigh);
        orderbook.ModifyOrder(OrderModify{ id, side, priceDist(rng), qtyDist(rng) });
    }

    // Resting orders to be modified in the measured phase, plus the
    // pre-built OrderModify requests (new price/quantity, same side).
    std::vector<OrderModify> modifications;
    modifications.reserve(kMeasuredOps);
    for (std::size_t i = 0; i < kMeasuredOps; ++i)
    {
        const OrderId id = nextId++;
        auto order = MakeNonCrossingOrder(id, rng, sideDist, qtyDist);
        const Side side = order->GetSide();
        const bool isBuy = side == Side::Buy;
        orderbook.AddOrder(std::move(order));

        std::uniform_int_distribution<Price> priceDist(isBuy ? kBidLow : kAskLow, isBuy ? kBidHigh : kAskHigh);
        modifications.emplace_back(id, side, priceDist(rng), qtyDist(rng));
    }

    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(kMeasuredOps);

    const auto batchStart = Clock::now();
    for (const auto& modification : modifications)
    {
        const auto opStart = Clock::now();
        orderbook.ModifyOrder(modification);
        const auto opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    const auto batchEnd = Clock::now();

    BenchResult result;
    result.name = "Modify Order";
    result.opCount = kMeasuredOps;
    result.elapsedSeconds = std::chrono::duration<double>(batchEnd - batchStart).count();
    result.latency = LatencyStats::Compute(std::move(latenciesNs));
    return result;
}

BenchResult BenchMatchOrder()
{
    using namespace BenchConfig;

    Orderbook orderbook;
    OrderId nextId = 1;

    // Warm-up: rest sell orders at the match price, then cross them with
    // buy orders that fully fill on arrival.
    for (std::size_t i = 0; i < kWarmupOps; ++i)
        orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, nextId++, Side::Sell, kMatchPrice, 1));
    for (std::size_t i = 0; i < kWarmupOps; ++i)
        orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, nextId++, Side::Buy, kMatchPrice, 1));

    // Resting sell orders for the measured phase, pre-built crossing buy
    // orders are constructed ahead of time so only AddOrder (insert + match)
    // is timed.
    for (std::size_t i = 0; i < kMeasuredOps; ++i)
        orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, nextId++, Side::Sell, kMatchPrice, 1));

    std::vector<OrderPointer> crossingOrders;
    crossingOrders.reserve(kMeasuredOps);
    for (std::size_t i = 0; i < kMeasuredOps; ++i)
        crossingOrders.push_back(std::make_shared<Order>(OrderType::GoodTillCancel, nextId++, Side::Buy, kMatchPrice, 1));

    std::vector<std::int64_t> latenciesNs;
    latenciesNs.reserve(kMeasuredOps);

    const auto batchStart = Clock::now();
    for (auto& order : crossingOrders)
    {
        const auto opStart = Clock::now();
        orderbook.AddOrder(order);
        const auto opEnd = Clock::now();
        latenciesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(opEnd - opStart).count());
    }
    const auto batchEnd = Clock::now();

    BenchResult result;
    result.name = "Match Order (full fill on arrival)";
    result.opCount = kMeasuredOps;
    result.elapsedSeconds = std::chrono::duration<double>(batchEnd - batchStart).count();
    result.latency = LatencyStats::Compute(std::move(latenciesNs));
    return result;
}

int main()
{
    const std::vector<BenchResult> results = {
        BenchAddOrder(),
        BenchCancelOrder(),
        BenchModifyOrder(),
        BenchMatchOrder(),
    };

    for (const auto& result : results)
        PrintResult(result);

    return 0;
}
