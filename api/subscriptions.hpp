#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <functional>

namespace asgard::api {

// ---------------------------------------------------------------------------
// SubscriptionManager
//
// Maps topic strings to sets of subscriber IDs.
// Thread-safe (called from WebSocket callbacks and engine thread).
//
// Topics follow the pattern from the spec:
//   Public:  depth.<instrument>, trade.<instrument>, markPrice.<instrument>, ...
//   Private: account.orders, account.positions, account.balances
// ---------------------------------------------------------------------------

using SubscriberId = uint64_t;

class SubscriptionManager {
public:
    void subscribe(SubscriberId id, const std::string& topic);
    void unsubscribe(SubscriberId id, const std::string& topic);
    void unsubscribe_all(SubscriberId id);

    // Get all subscriber IDs for a topic
    std::vector<SubscriberId> get_subscribers(const std::string& topic) const;

    // Authenticate a subscriber → user_id mapping
    void authenticate(SubscriberId id, const std::string& user_id);
    std::string get_user_id(SubscriberId id) const;
    bool is_authenticated(SubscriberId id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::unordered_set<SubscriberId>> topic_subs_;
    std::unordered_map<SubscriberId, std::unordered_set<std::string>> sub_topics_;
    std::unordered_map<SubscriberId, std::string> auth_map_; // id → user_id
};

} // namespace asgard::api
