#include <iostream>
#include <map>
#include <set>
#include <list>
#include <cmath>
#include <functional>
#include <ctime>
#include <deque>
#include <queue>
#include <stack>
#include <limits>
#include <string>
#include <vector>
#include <numeric>
#include <algorithm>
#include <unordered_map>
#include <memory>
#include <variant>
#include <optional>
#include <tuple>
#include <format>


enum class OrderType{
  GoodTillCancel,
  FillAndKill
};

enum class Side{
  Buy,
  Sell
};

using Price = std::int; // alias for readability
using Quantity = std::u_int32_t;
using OrderId = std::u_int64_t;

struct LevelInfo{
  Price price_;
  Quantity quantity_;
}

using LevelInfos = std::vector<LevelInfo>

class OrderbookLevelInfos{
public:
  OrderBookLevelInfos(const LevelInfos& bids, const LevelInfos& asks)
    : bids_{ bids }, asks_{ asks } { }

  const LevelInfos& GetBids() const { return bids_; }
  const LevelInfos& GetAsks() const { return asks_; }

private:
  LevelInfos bids_;
  LevelInfos asks_;
}

class Order{
public:
  Order(OrderType orderType, OrderId orderId, Side side, Price price, Quantity quantity)
    : orderType_ { orderType }
    , orderId_ { orderId }
    , side_ { side }
    , price {price}
    , initialQuantity_ { quantity }
    , remainingQuantity_ {quantity}
    { }

    OrderId_ GetOrderId() const { return orderId_; }
    Side GetSide() const { return side_; }
    Price GetPrice() const { return price_; }
    OrderType GetOrderType() const { return orderType_; }
    Quantity GetInitialQuantity() const { return initialQuantity_; }
    Quantity GetRemainingQuantity() const { return remainingQuantity_; }
    Quantity GetFilledQuantity() const { return GetInitialQuantity() - GetRemainingQuantity(); }

    void Fill(Quantity quantity){
      if (quantity > GetRemainingQuantity()) // safety checkj
        throw std::logic_error(std::format("Order ({}) cannot be filled for more than its remaining quantity.", GetOrderId()));
      
      remainingQuantity_ -= quantity;
    }
private:
  OrderType orderType_;
  OrderId orderId_;
  Side side_;
  Price price_;
  Quantity initialQuantity_;
  Quantity remainingQuantity_;
};

/* choose shared ptr because we want to use a signle Order in multiple data structures
 * we want refernece semantics
 * Order can be stored in orders dictionary, and can be stored in a bids/asks dictionary
 */

using OrderPointer = std::shared_ptr<Order>;
// Choose a list for now, can be potentially optimized with a std::vector
using OrderPointers = std::list<OrderPointer>;

/* modifying an order is more complicated than just adding/removing an order
* abstraction of an order to be modified. To actually modify, we cance and then replace.
* Cancel: get by Id. Replace requires price and quantity (and also side) so we can change them.
*/


class OrderModify{
public:
  OrderModify(OrderId orderId, Side side, Price price, Quantity quantity)
    : orderId_ { orderId }
    , side_ { side }
    , price_ { price }
    , quantity_ { quantity }
  { }
  
  OrderId GetOrderId() const { return orderId_; }
  Price GetPrice() const { return price_; }
  Side GetSide() const { return side_; }
  Quantity GetQuantity() const { return quantity_; }
 
  // converts an existing order into a new modified order
  OrderPointer ToOrderPointer(OrderType type) const {
    return std::make_shared<Order>(type, GetOrderId(), GetSide(), GetPrice(), GetQuantity());
  }

private:
  OrderId orderId_;
  Side side_;
  Price price_;
  Quantity quantity_;
};

struct TradeInfo{
  OrderId orderId_;
  Price price_;
  Quantity quantity_;
};

// Trade object is a representation of 2 tradeinfo objects: 1 bid, 1 ask

class Trade{
public:
  Trade(const TradeInfo& bidTrade, const TradeInfo& askTrade)
    : bidTrade_ { bidTrade }
    , askTrade_ { askTrade }
  { }

  const TradeInfo& GetBidTrade() const { return bidTrade_; }
  const TradeInfo& GetAskTrade() const { return askTrade_; }

private:
  TradeInfo bidTrade_;
  TradeInfo askTrade_;
};

// we want a vector of trades because there can be many different trades to match

using Trades = std::vector<Trade>;

class OrderBook{
private:
  /* use a map to repr bids and ask.
   * bids sorted in descending order from best to worst
   * asks sorted in ascending order from best to worst
   * Have O(1) access to orders by id
   */

  struct OrderEntry{
    OrderPointer order_ { nullptr };
    OrderPointers::iterator location_;
  };

  // using std::greater, we store keys in descending order, less is oppo
  std::map<Price, OrderPointers, std::greater<Price>> bids_;
  std::map<Price, OrderPointers, std::less<Price>> asks_;

  std::unordered_map<OrderId, OrderEntry> orders_;

};

int main(){

  return 0;
}


