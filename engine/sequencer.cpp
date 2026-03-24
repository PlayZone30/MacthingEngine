#include "sequencer.hpp"
#include "models.hpp"

namespace asgard {

uint64_t Sequencer::next(const std::string& event_type,
                          const std::string& payload_json) {
    uint64_t seq = counter_.fetch_add(1, std::memory_order_relaxed) + 1;
    wal_.push_back(WalEntry{seq, event_type, payload_json, now_us()});
    return seq;
}

void Sequencer::replay(std::function<void(const WalEntry&)> handler) const {
    for (const auto& entry : wal_) {
        handler(entry);
    }
}

} // namespace asgard
