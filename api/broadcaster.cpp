#include "broadcaster.hpp"

namespace asgard::api {

void Broadcaster::publish(const std::string& topic, const nlohmann::json& data) {
    if (!send_fn_) return;

    nlohmann::json envelope = {
        {"stream", topic},
        {"data",   data}
    };
    std::string payload = envelope.dump();

    auto subscribers = subs_.get_subscribers(topic);
    for (SubscriberId id : subscribers) {
        send_fn_(id, payload);
    }
}

void Broadcaster::publish_private(const std::string& user_id,
                                   const std::string& channel,
                                   const nlohmann::json& data) {
    if (!send_fn_) return;

    // Private topics are of the form: account.orders, account.positions, etc.
    // We re-use the public subscription mechanism; the topic key is
    // "<channel>.<user_id>" internally so each user sees only their data.
    std::string topic = channel + "." + user_id;

    nlohmann::json envelope = {
        {"stream", channel},
        {"data",   data}
    };
    std::string payload = envelope.dump();

    auto subscribers = subs_.get_subscribers(topic);
    for (SubscriberId id : subscribers) {
        send_fn_(id, payload);
    }
}

} // namespace asgard::api
