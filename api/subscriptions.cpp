#include "subscriptions.hpp"

namespace asgard::api {

void SubscriptionManager::subscribe(SubscriberId id, const std::string& topic) {
    std::lock_guard<std::mutex> lk(mutex_);
    topic_subs_[topic].insert(id);
    sub_topics_[id].insert(topic);
}

void SubscriptionManager::unsubscribe(SubscriberId id, const std::string& topic) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = topic_subs_.find(topic);
    if (it != topic_subs_.end()) {
        it->second.erase(id);
        if (it->second.empty()) topic_subs_.erase(it);
    }
    auto it2 = sub_topics_.find(id);
    if (it2 != sub_topics_.end()) {
        it2->second.erase(topic);
    }
}

void SubscriptionManager::unsubscribe_all(SubscriberId id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = sub_topics_.find(id);
    if (it == sub_topics_.end()) return;
    for (const auto& topic : it->second) {
        auto tit = topic_subs_.find(topic);
        if (tit != topic_subs_.end()) {
            tit->second.erase(id);
            if (tit->second.empty()) topic_subs_.erase(tit);
        }
    }
    sub_topics_.erase(it);
    auth_map_.erase(id);
}

std::vector<SubscriberId> SubscriptionManager::get_subscribers(const std::string& topic) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = topic_subs_.find(topic);
    if (it == topic_subs_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

void SubscriptionManager::authenticate(SubscriberId id, const std::string& user_id) {
    std::lock_guard<std::mutex> lk(mutex_);
    auth_map_[id] = user_id;
}

std::string SubscriptionManager::get_user_id(SubscriberId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = auth_map_.find(id);
    return (it != auth_map_.end()) ? it->second : "";
}

bool SubscriptionManager::is_authenticated(SubscriberId id) const {
    std::lock_guard<std::mutex> lk(mutex_);
    return auth_map_.count(id) > 0;
}

} // namespace asgard::api
