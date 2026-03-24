#pragma once

#include "subscriptions.hpp"
#include "broadcaster.hpp"
#include "../engine/models.hpp"
#include "concurrentqueue.h"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>

namespace asgard::api {

// ---------------------------------------------------------------------------
// WebSocketServer
//
// Wraps uWebSockets App to expose:
//   - WebSocket endpoint "/*"  for streaming + order submission
//   - REST GET /api/v1/depth
//   - REST GET /api/v1/ticker
//   - REST GET /api/v1/accounts/:user_id  (auth required)
//
// Per-socket state:
//   - subscriber_id  (auto-assigned monotonic uint64)
//   - authenticated  (via "auth" JSON-RPC call)
//
// The server runs on the calling thread (uWS event loop via .run()).
// It blocks until stop() is called from another thread.
//
// Order submission:
//   method="place_order"  → validates JSON, pushes into order_queue
//   method="cancel_order" → validated, pushed as special cancel sentinel
//   method="SUBSCRIBE"    → registers topic subscriptions
//   method="UNSUBSCRIBE"  → removes topic subscriptions
//   method="auth"         → stores user_id mapping
// ---------------------------------------------------------------------------

// Callbacks the server needs from the engine layer
struct EngineInterface {
    // L2 snapshot function (called for REST depth endpoint)
    std::function<nlohmann::json(int depth)> get_depth;
    // Ticker (last price, 24h vol etc.)
    std::function<nlohmann::json()>          get_ticker;
    // Account info for authenticated user
    std::function<nlohmann::json(const std::string& user_id)> get_account;
};

class WebSocketServer {
public:
    WebSocketServer(moodycamel::ConcurrentQueue<Order>& order_queue,
                    SubscriptionManager& subs,
                    Broadcaster& broadcaster,
                    EngineInterface iface,
                    int port = 9001);

    // Blocking — runs the uWS event loop.
    void run();

    // Thread-safe stop (signals the loop to exit).
    void stop();

    // Called by the engine thread to push data to a specific subscriber.
    // Broadcaster uses this via the registered send_fn.
    void send_to(SubscriberId id, const std::string& payload);

private:
    void handle_message(SubscriberId id, std::string_view msg);
    void handle_subscribe(SubscriberId id, const nlohmann::json& j);
    void handle_unsubscribe(SubscriberId id, const nlohmann::json& j);
    void handle_place_order(SubscriberId id, const nlohmann::json& j);
    void handle_cancel_order(SubscriberId id, const nlohmann::json& j);
    void handle_auth(SubscriberId id, const nlohmann::json& j);

    Order parse_order(const nlohmann::json& j, const std::string& user_id);

    moodycamel::ConcurrentQueue<Order>& order_queue_;
    SubscriptionManager&                subs_;
    Broadcaster&                        broadcaster_;
    EngineInterface                     iface_;
    int                                 port_;

    // id → uWS WebSocket pointer (opaque void* to avoid uWS header dep here)
    std::mutex                                  ws_mutex_;
    std::unordered_map<SubscriberId, void*>     id_to_ws_;
    std::unordered_map<void*, SubscriberId>     ws_to_id_;
    std::atomic<SubscriberId>                   next_id_{1};

    // uWS loop handle (opaque)
    void* loop_ = nullptr;
    std::atomic<bool> running_{false};
};

} // namespace asgard::api
