#include "Orderbook.cpp"
#include <thread>
#include <atomic>
int main(){
    Orderbook ob;
    std::atomic<bool> stop{false};
    std::thread reader([&]{ while(!stop) (void)ob.GetOrderInfos(); });
    for(OrderId id=1; id<50000; ++id){
        ob.AddOrder(std::make_shared<Order>(OrderType::GoodTillCancel, id,
                    (id&1)?Side::Buy:Side::Sell, 10000+(id%50), 10));
        if(id%3==0) ob.CancelOrder(id-1);
    }
    stop=true; reader.join();
}
