#pragma once

#include "order_pool.h"
#include "price_level.h"

#include <cassert>
#include <unordered_map>

namespace detail {
    constexpr std::size_t kDefaultMaxPriceLevels = 4096;
}

template<typename TradeCallback>
class OrderBook {
public:
    OrderBook(std::size_t capacity, TradeCallback callback)
        : pool_(capacity), onTrade_(std::move(callback))
    {
        // Pre-allocate to avoid heap allocations on hot path
        orderIndex_.max_load_factor(0.7f);
        orderIndex_.reserve(capacity);
        bids_.reserve(detail::kDefaultMaxPriceLevels);
        asks_.reserve(detail::kDefaultMaxPriceLevels);
    }

    void addLimitOrder(Side side, uint32_t price, uint32_t quantity, uint64_t id, uint64_t participantId) {
        Order* order = pool_.allocate();
        order->init(id, price, quantity, sequence_++, side, participantId);

        if (side == Side::Buy) {
            if (bestAsk() != nullptr && price >= bestAsk()->price) {
                matchBuy(order);
            }
        } else if (side == Side::Sell) {
            if (bestBid() != nullptr && price <= bestBid()->price) {
                matchSell(order);
            }
        }

        if (order->quantity > 0) {
            if (side == Side::Buy) {
                PriceLevel* pl = findOrCreateBidLevel(price);
                pl->addToTail(order);
            } else if (side == Side::Sell) {
                PriceLevel* pl = findOrCreateAskLevel(price);
                pl->addToTail(order);
            }
            orderIndex_.try_emplace(id, order);
        } else {
            pool_.deallocate(order);
        }
    }

    void cancelOrder(uint64_t orderId) {
        auto it = orderIndex_.find(orderId);

        if (it == orderIndex_.end()) {
            return;
        }

        Order* o = it->second;
        assert(o && o->quantity > 0);

        auto& levels = (o->side == Side::Buy) ? bids_ : asks_;
        auto levelIt = (o->side == Side::Buy) ? findBidLevel(o->price) : findAskLevel(o->price);

        assert(levelIt != levels.end() && levelIt->price == o->price && "Order in index but price level missing");

        levelIt->remove(o);

        if (levelIt->isEmpty()) {
            levels.erase(levelIt);
        }

        orderIndex_.erase(it);
        pool_.deallocate(o);
    }

    const PriceLevel* bestBid() const {
        return bids_.empty() ? nullptr : &bids_.back();
    }

    const PriceLevel* bestAsk() const {
        return asks_.empty() ? nullptr : &asks_.back();
    }

private:
    OrderPool pool_;
    TradeCallback onTrade_;
    std::vector<PriceLevel> bids_; // ascending
    std::vector<PriceLevel> asks_; // descending
    std::unordered_map<uint64_t, Order*> orderIndex_;
    uint64_t sequence_ = 0;

    void matchBuy(Order* incoming) {
        if (asks_.empty()) return;
        
        // Cache: avoid repeated .size()/.empty()/.back() calls
        auto* levels = asks_.data();
        auto numLevels = asks_.size();
        auto* pl = levels + numLevels - 1;  // best ask (back)
        const auto incomingPrice = incoming->price;
        
        while (incoming->quantity > 0 && numLevels > 0) {
            if (incomingPrice < pl->price) { break; }

            Order* resting = pl->head;  // inlined front()

            if (resting->participantId == incoming->participantId) {
                incoming->quantity = 0;
                break;  // defer cleanup to end
            }

            const uint32_t fillQty = std::min(incoming->quantity, resting->quantity);

            incoming->quantity -= fillQty;
            resting->quantity -= fillQty;
            pl->totalQuantity -= fillQty;

            onTrade_(Trade{incoming->orderId, resting->orderId, pl->price, fillQty});

            if (resting->quantity == 0) {
                pl->remove(resting);
                orderIndex_.erase(resting->orderId);
                pool_.deallocate(resting);
            }

            if (pl->head == nullptr) {  // inlined isEmpty()
                --numLevels;
                --pl;
            }
        }
        
        // Batch resize: single operation instead of repeated pop_back()
        asks_.resize(numLevels);
    }

    void matchSell(Order* incoming) {
        if (bids_.empty()) return;
        
        // Cache: avoid repeated .size()/.empty()/.back() calls
        auto* levels = bids_.data();
        auto numLevels = bids_.size();
        auto* pl = levels + numLevels - 1;  // best bid (back)
        const auto incomingPrice = incoming->price;
        
        while (incoming->quantity > 0 && numLevels > 0) {
            if (incomingPrice > pl->price) { break; }

            Order* resting = pl->head;  // inlined front()

            if (resting->participantId == incoming->participantId) {
                incoming->quantity = 0;
                break;  // defer cleanup to end
            }

            const uint32_t fillQty = std::min(incoming->quantity, resting->quantity);

            incoming->quantity -= fillQty;
            resting->quantity -= fillQty;
            pl->totalQuantity -= fillQty;

            onTrade_(Trade{resting->orderId, incoming->orderId, pl->price, fillQty});

            if (resting->quantity == 0) {
                pl->remove(resting);
                orderIndex_.erase(resting->orderId);
                pool_.deallocate(resting);
            }

            if (pl->head == nullptr) {  // inlined isEmpty()
                --numLevels;
                --pl;
            }
        }
        
        // Batch resize: single operation instead of repeated pop_back()
        bids_.resize(numLevels);
    }

    auto findBidLevel(uint32_t price) -> std::vector<PriceLevel>::iterator {
        return std::lower_bound(bids_.begin(), bids_.end(), price,
            [](const PriceLevel& pl, uint32_t p) { return pl.price < p; });
    }

    PriceLevel* findOrCreateBidLevel(uint32_t price) {
        auto it = findBidLevel(price);
        if (it != bids_.end() && it->price == price) {
            return &(*it);
        }
        assert(bids_.size() < bids_.capacity() && "bids_ capacity exceeded - would reallocate");
        it = bids_.insert(it, PriceLevel(price));
        return &(*it);
    }

    auto findAskLevel(uint32_t price) -> std::vector<PriceLevel>::iterator {
        return std::lower_bound(asks_.begin(), asks_.end(), price,
            [](const PriceLevel& pl, uint32_t p) { return pl.price > p; });
    }

    PriceLevel* findOrCreateAskLevel(uint32_t price) {
        auto it = findAskLevel(price);
        if (it != asks_.end() && it->price == price) {
            return &(*it);
        }
        assert(asks_.size() < asks_.capacity() && "asks_ capacity exceeded - would reallocate");
        it = asks_.insert(it, PriceLevel(price));
        return &(*it);
    }
};
