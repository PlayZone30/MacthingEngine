#pragma once

#include "subscriptions.hpp"
#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <unordered_map>
#include <mutex>

namespace asgard::api {

// ---------------------------------------------------------------------------
// Broadcaster
//
// Fans out JSON payloads to all subscribers of a topic.
// WebSocket send is abstracted via a registered callback so the broadcaster
// has no direct dependency on uWebSockets types (easier to test).
//
// The engine thread calls publish() after each trade / book update.
// ---------------------------------------------------------------------------

// send_fn(subscriber_id, payload_string)
using SendFn = std::function<void(SubscriberId, const std::string&)>;

class Broadcaster {
public:
    explicit Broadcaster(SubscriptionManager& subs) : subs_(subs) {}

    // Register the WebSocket send function (called once at startup).
    void set_send_fn(SendFn fn) { send_fn_ = std::move(fn); }

    // Publish a message to all subscribers of the topic.
    void publish(const std::string& topic, const nlohmann::json& data);

    // Publish a private message to a specific user (all their private subs).
    void publish_private(const std::string& user_id,
                         const std::string& channel,
                         const nlohmann::json& data);

private:
    SubscriptionManager& subs_;
    SendFn               send_fn_;
};

} // namespace asgard::api
