#include "websocket_server.hpp"
#include <App.h>
#include <iostream>
#include <sstream>
#include <iomanip>

namespace asgard::api {

// Per-connection user data stored inside uWS
struct PerSocketData {
    SubscriberId id = 0;
};

WebSocketServer::WebSocketServer(moodycamel::ConcurrentQueue<Order>& order_queue,
                                  SubscriptionManager& subs,
                                  Broadcaster& broadcaster,
                                  EngineInterface iface,
                                  int port)
    : order_queue_(order_queue)
    , subs_(subs)
    , broadcaster_(broadcaster)
    , iface_(std::move(iface))
    , port_(port)
{}

// ---------------------------------------------------------------------------
// run — blocks on the uWS event loop
// ---------------------------------------------------------------------------

void WebSocketServer::run() {
    running_.store(true);

    // Register the send_fn with broadcaster
    broadcaster_.set_send_fn([this](SubscriberId id, const std::string& payload) {
        send_to(id, payload);
    });

    uWS::App app;

    // -----------------------------------------------------------------------
    // WebSocket handler
    // -----------------------------------------------------------------------
    app.ws<PerSocketData>("/*", {
        .compression = uWS::SHARED_COMPRESSOR,
        .maxPayloadLength = 64 * 1024,
        .idleTimeout = 120,

        .open = [this](uWS::WebSocket<false, true, PerSocketData>* ws) {
            SubscriberId id = next_id_.fetch_add(1);
            ws->getUserData()->id = id;
            {
                std::lock_guard<std::mutex> lk(ws_mutex_);
                id_to_ws_[id] = static_cast<void*>(ws);
                ws_to_id_[static_cast<void*>(ws)] = id;
            }
        },

        .message = [this](uWS::WebSocket<false, true, PerSocketData>* ws,
                          std::string_view message, uWS::OpCode /*opCode*/) {
            SubscriberId id = ws->getUserData()->id;
            handle_message(id, message);
        },

        .close = [this](uWS::WebSocket<false, true, PerSocketData>* ws,
                        int /*code*/, std::string_view /*message*/) {
            SubscriberId id = ws->getUserData()->id;
            subs_.unsubscribe_all(id);
            {
                std::lock_guard<std::mutex> lk(ws_mutex_);
                id_to_ws_.erase(id);
                ws_to_id_.erase(static_cast<void*>(ws));
            }
        }
    });

    // -----------------------------------------------------------------------
    // REST: GET /api/v1/depth?depth=100
    // -----------------------------------------------------------------------
    app.get("/api/v1/depth", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
        int depth = 100;
        std::string_view dv = req->getQuery("depth");
        if (!dv.empty()) {
            try { depth = std::stoi(std::string(dv)); } catch (...) {}
        }
        nlohmann::json snap = iface_.get_depth(depth);
        res->writeHeader("Content-Type", "application/json")
           ->end(snap.dump());
    });

    // -----------------------------------------------------------------------
    // REST: GET /api/v1/ticker
    // -----------------------------------------------------------------------
    app.get("/api/v1/ticker", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest*) {
        nlohmann::json ticker = iface_.get_ticker();
        res->writeHeader("Content-Type", "application/json")
           ->end(ticker.dump());
    });

    // -----------------------------------------------------------------------
    // REST: GET /api/v1/account?user_id=XXX
    // -----------------------------------------------------------------------
    app.get("/api/v1/account", [this](uWS::HttpResponse<false>* res, uWS::HttpRequest* req) {
        std::string user_id = std::string(req->getQuery("user_id"));
        if (user_id.empty()) {
            res->writeStatus("400 Bad Request")->end("{\"error\":\"missing user_id\"}");
            return;
        }
        nlohmann::json acc = iface_.get_account(user_id);
        res->writeHeader("Content-Type", "application/json")
           ->end(acc.dump());
    });

    // -----------------------------------------------------------------------
    // Listen and run
    // -----------------------------------------------------------------------
    app.listen(port_, [this](us_listen_socket_t* token) {
        if (token) {
            loop_ = token;
            std::cout << "[WS] Listening on port " << port_ << "\n";
        } else {
            std::cerr << "[WS] Failed to listen on port " << port_ << "\n";
        }
    });

    app.run(); // blocks

    running_.store(false);
}

void WebSocketServer::stop() {
    running_.store(false);
    if (loop_) {
        us_listen_socket_close(0, static_cast<us_listen_socket_t*>(loop_));
        loop_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// send_to — called from engine thread (via Broadcaster)
// ---------------------------------------------------------------------------

void WebSocketServer::send_to(SubscriberId id, const std::string& payload) {
    void* raw_ws = nullptr;
    {
        std::lock_guard<std::mutex> lk(ws_mutex_);
        auto it = id_to_ws_.find(id);
        if (it == id_to_ws_.end()) return;
        raw_ws = it->second;
    }
    // uWS is not thread-safe for send. We defer via the loop.
    // In production, use uWS::Loop::defer(). For simulation single-process
    // usage, the engine thread sends synchronously — safe when the loop
    // and engine thread are coordinated via the main loop.
    auto* ws = static_cast<uWS::WebSocket<false, true, PerSocketData>*>(raw_ws);
    ws->send(payload, uWS::OpCode::TEXT);
}

// ---------------------------------------------------------------------------
// Message dispatch
// ---------------------------------------------------------------------------

void WebSocketServer::handle_message(SubscriberId id, std::string_view msg) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(msg);
    } catch (...) {
        return; // silently drop malformed JSON
    }

    std::string method = j.value("method", "");
    if      (method == "SUBSCRIBE")   handle_subscribe(id, j);
    else if (method == "UNSUBSCRIBE") handle_unsubscribe(id, j);
    else if (method == "place_order") handle_place_order(id, j);
    else if (method == "cancel_order") handle_cancel_order(id, j);
    else if (method == "auth")        handle_auth(id, j);
}

void WebSocketServer::handle_subscribe(SubscriberId id, const nlohmann::json& j) {
    if (!j.contains("params") || !j["params"].is_array()) return;
    for (const auto& topic : j["params"]) {
        if (!topic.is_string()) continue;
        std::string t = topic.get<std::string>();
        // Private topics require auth
        if (t.rfind("account.", 0) == 0 && !subs_.is_authenticated(id)) continue;
        // For private topics, register as account.<channel>.<user_id>
        if (t.rfind("account.", 0) == 0) {
            std::string user_id = subs_.get_user_id(id);
            subs_.subscribe(id, t + "." + user_id);
        } else {
            subs_.subscribe(id, t);
        }
    }
}

void WebSocketServer::handle_unsubscribe(SubscriberId id, const nlohmann::json& j) {
    if (!j.contains("params") || !j["params"].is_array()) return;
    for (const auto& topic : j["params"]) {
        if (topic.is_string()) subs_.unsubscribe(id, topic.get<std::string>());
    }
}

void WebSocketServer::handle_place_order(SubscriberId id, const nlohmann::json& j) {
    if (!subs_.is_authenticated(id)) return;
    std::string user_id = subs_.get_user_id(id);
    try {
        Order o = parse_order(j, user_id);
        order_queue_.enqueue(o);
    } catch (const std::exception& e) {
        // Could send an error response back; omitted for brevity
        (void)e;
    }
}

void WebSocketServer::handle_cancel_order(SubscriberId id, const nlohmann::json& j) {
    if (!subs_.is_authenticated(id)) return;
    std::string user_id = subs_.get_user_id(id);
    std::string order_id = j.value("order_id", "");
    if (order_id.empty()) return;

    // Encode a cancel request as a special Order with status=CANCELLED
    Order cancel_sentinel;
    cancel_sentinel.order_id = order_id;
    cancel_sentinel.user_id  = user_id;
    cancel_sentinel.status   = OrderStatus::CANCELLED; // signals cancel intent
    order_queue_.enqueue(cancel_sentinel);
}

void WebSocketServer::handle_auth(SubscriberId id, const nlohmann::json& j) {
    // Simple API-key authentication — in production, verify JWT/HMAC.
    // For simulation, we trust the provided user_id.
    std::string user_id = j.value("user_id", "");
    if (user_id.empty()) return;
    subs_.authenticate(id, user_id);
}

// ---------------------------------------------------------------------------
// Order parsing from JSON-RPC payload
// ---------------------------------------------------------------------------

Order WebSocketServer::parse_order(const nlohmann::json& j, const std::string& user_id) {
    Order o;
    o.user_id    = user_id;
    o.instrument = j.at("instrument").get<std::string>();
    o.timestamp_us = now_us();
    o.status     = OrderStatus::NEW;

    // side
    std::string side_str = j.at("side").get<std::string>();
    o.side = (side_str == "BUY") ? OrderSide::BUY : OrderSide::SELL;

    // type
    std::string type_str = j.value("type", "LIMIT");
    if      (type_str == "MARKET")    o.type = OrderType::MARKET;
    else if (type_str == "STOP_LIMIT") o.type = OrderType::STOP_LIMIT;
    else if (type_str == "POST_ONLY") o.type = OrderType::POST_ONLY;
    else                               o.type = OrderType::LIMIT;

    o.price        = j.value("price", 0.0);
    o.quantity     = j.at("quantity").get<double>();
    o.remaining_qty = o.quantity;

    // TIF
    std::string tif_str = j.value("tif", "GTC");
    if      (tif_str == "IOC") o.tif = TIF::IOC;
    else if (tif_str == "FOK") o.tif = TIF::FOK;
    else                        o.tif = TIF::GTC;

    o.reduce_only = j.value("reduce_only", false);

    // STP
    std::string stp_str = j.value("stp_mode", "CANCEL_INCOMING");
    if      (stp_str == "CANCEL_RESTING") o.stp_mode = STPMode::CANCEL_RESTING;
    else if (stp_str == "CANCEL_BOTH")    o.stp_mode = STPMode::CANCEL_BOTH;
    else                                  o.stp_mode = STPMode::CANCEL_INCOMING;

    // Margin mode
    std::string mm_str = j.value("margin_mode", "CROSS");
    o.margin_mode = (mm_str == "ISOLATED") ? MarginMode::ISOLATED : MarginMode::CROSS;

    // Generate order_id
    static std::atomic<uint64_t> ws_order_ctr{1};
    std::ostringstream oss;
    oss << "WS-" << std::setfill('0') << std::setw(10) << ws_order_ctr.fetch_add(1);
    o.order_id = oss.str();

    return o;
}

} // namespace asgard::api
