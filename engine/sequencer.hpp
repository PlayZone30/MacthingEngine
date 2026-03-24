#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>

namespace asgard {

// ---------------------------------------------------------------------------
// WAL entry
// ---------------------------------------------------------------------------

struct WalEntry {
    uint64_t    seq;
    std::string event_type;  // NEW_ORDER, CANCEL, MODIFY, MASS_QUOTE, TRADE, ADMIN
    std::string payload_json;
    uint64_t    timestamp_us;
};

// ---------------------------------------------------------------------------
// Sequencer
//
// Single-writer design:
//   - next() MUST be called only from the engine thread.
//   - The atomic counter is still used so that stats threads can read it.
// ---------------------------------------------------------------------------

class Sequencer {
public:
    // Assign the next sequence number and append to the WAL.
    // Returns the assigned sequence number.
    uint64_t next(const std::string& event_type, const std::string& payload_json);

    // Current counter value (non-incrementing read)
    uint64_t current() const { return counter_.load(std::memory_order_relaxed); }

    // Replay WAL — calls handler for each entry in sequence order.
    // Used for crash recovery.
    void replay(std::function<void(const WalEntry&)> handler) const;

    // WAL size (number of events recorded)
    std::size_t wal_size() const { return wal_.size(); }

private:
    std::atomic<uint64_t> counter_{0};
    std::vector<WalEntry> wal_;        // in-memory WAL (swap for file-based in production)
};

} // namespace asgard
