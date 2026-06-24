#include "../Orderbook.cpp"

#include <chrono>
#include <iostream>
#include <random>

using Clock = std::chrono::steady_clock;

template <typename Fn>
double TimeMs(Fn&& fn)
{
    const auto start = Clock::now();
    fn();
    const auto end = Clock::now();
    return std::chrono::duration<double, std::milli>(end - start).count();
}

int main()
{
    constexpr std::size_t numOrders = 200'000;

    std::mt19937 rng{ 42 };
    std::uniform_int_distribution<Price> priceDist{ 9'900, 10'100 };
    std::uniform_int_distribution<int> sideDist{ 0, 1 };
    std::uniform_int_distribution<Quantity> qtyDist{ 1, 100 };

    Orderbook orderbook;
    OrderId nextId = 1;

    const auto addMs = TimeMs([&]
    {
        for (std::size_t i = 0; i < numOrders; ++i)
        {
            const auto side = sideDist(rng) == 0 ? Side::Buy : Side::Sell;
            const auto price = priceDist(rng);
            const auto quantity = qtyDist(rng);
            orderbook.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, nextId++, side, price, quantity));
        }
    });

    std::cout << "Added " << numOrders << " orders in " << addMs << " ms ("
              << (numOrders / (addMs / 1000.0)) << " orders/sec)\n";
    std::cout << "Resting orders after add phase: " << orderbook.Size() << "\n";

    OrderId cancelId = 1;
    const auto cancelMs = TimeMs([&]
    {
        for (std::size_t i = 0; i < numOrders; ++i)
            orderbook.CancelOrder(cancelId++);
    });

    std::cout << "Cancelled " << numOrders << " ids in " << cancelMs << " ms ("
              << (numOrders / (cancelMs / 1000.0)) << " cancels/sec)\n";
    std::cout << "Resting orders after cancel phase: " << orderbook.Size() << "\n";

    return 0;
}
