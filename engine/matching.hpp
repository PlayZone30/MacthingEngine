#pragma once

#include "models.hpp"
#include "orderbook.hpp"
#include "sequencer.hpp"
#include "margin.hpp"
#include <vector>
#include <functional>
#include <string>
#include <unordered_map>
#include <atomic>

namespace asgard {

// ---------------------------------------------------------------------------
// MatchingEngine
//
// Single-threaded.  process() must only be called from the engine thread.
// The engine emits Trade events which are then processed by the post-trade
// pipeline (positions.cpp).
//
// STP is evaluated here at match time (not as a pre-trade check).
// Layer 2 (price impact band) is enforced per fill here.
// ---------------------------------------------------------------------------

class MatchingEngine {
public:
    explicit MatchingEngine(OrderBook& book,
                            Sequencer& seq,
                            const Instrument& inst,
                            const std::unordered_map<std::string, UserAccount>& accounts)
        : book_(book), seq_(seq), inst_(inst), accounts_(accounts)
    {}

    // Process an incoming order.  Returns all trades produced.
    // The caller is responsible for post-trade position/margin updates.
    std::vector<Trade> process(Order& incoming, double mark_price);

    // Cancel a resting order.  Returns true if found.
    bool cancel(const std::string& order_id,
                UserAccount& user,
                const std::string& instrument);

    // Modify (cancel-replace): cancel old, return new order for re-submission.
    // The caller must re-submit through the full pipeline (risk checks etc.).
    // Returns false if old order not found.
    bool cancel_for_modify(const std::string& old_order_id, UserAccount& user);

    // Statistics
    uint64_t total_trades() const { return trade_counter_.load(); }

private:
    // Walk the opposite side and fill as much as possible.
    // Returns fills produced.
    std::vector<Trade> try_match(Order& incoming, double mark_price);

    // Check Layer 2: does this fill price violate the impact band?
    bool violates_impact_band(double fill_price,
                               double arrival_best,
                               OrderSide aggressive_side) const;

    // Handle Self-Trade Prevention.  Returns true if the incoming order
    // should continue (resting was cancelled), false if incoming is cancelled.
    bool handle_stp(Order& incoming, Order& resting,
                    UserAccount& incoming_user, UserAccount& resting_user);

    // Apply a fill to both orders (reduce remaining_qty, update status).
    Trade make_trade(const Order& incoming, const Order& resting, double fill_qty);

    // Release open_order_margin when a resting order is cancelled or filled.
    void release_order_margin(UserAccount& user, const Order& o, double qty);

    // Reserve open_order_margin when a GTC order rests in the book.
    void reserve_order_margin(UserAccount& user, const Order& o, double mark_price);

    OrderBook& book_;
    Sequencer& seq_;
    const Instrument& inst_;
    const std::unordered_map<std::string, UserAccount>& accounts_;

    uint64_t trade_seq_ = 0;  // local trade counter for trade_id generation
    std::atomic<uint64_t> trade_counter_{0};
};

} // namespace asgard
