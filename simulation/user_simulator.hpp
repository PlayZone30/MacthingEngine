#pragma once

#include "../engine/models.hpp"
#include "user_profiles.hpp"
#include "order_generator.hpp"
#include "market_price.hpp"
#include "concurrentqueue.h"
#include <vector>
#include <thread>
#include <atomic>
#include <string>
#include <memory>
#include <random>
#include <functional>

namespace asgard::sim {

// ---------------------------------------------------------------------------
// UserSimulator
//
// Manages 10,000 simulated users spread across a thread pool.
// Each thread simulates a subset of users by sleeping the appropriate
// inter-arrival time between orders.
//
// Architecture:
//   - N threads (hardware_concurrency) each simulate M = 10000/N users
//   - Each thread has its own OrderGenerator (no shared state)
//   - All threads push to a single lock-free ConcurrentQueue<Order>
//   - The engine thread drains the queue
// ---------------------------------------------------------------------------

struct SimUser {
    UserAccount account;
    UserProfile profile;
    double      next_order_time_s = 0.0; // seconds since sim start
};

class UserSimulator {
public:
    UserSimulator(moodycamel::ConcurrentQueue<Order>& queue,
                  GBMSimulator& market,
                  const std::string& instrument,
                  int num_users = 10'000)
        : queue_(queue), market_(market), instrument_(instrument)
    {
        init_users(num_users);
    }

    // Start all simulation threads
    void start();

    // Stop all threads gracefully
    void stop();

    // Access user accounts (engine thread reads/writes these)
    std::unordered_map<std::string, UserAccount>& accounts() { return accounts_; }
    const std::unordered_map<std::string, UserAccount>& accounts() const { return accounts_; }

    // Returns vector of raw pointers to all accounts (for risk monitor)
    std::vector<UserAccount*> account_ptrs();

    int user_count() const { return static_cast<int>(users_.size()); }
    double total_deposited() const { return total_deposited_; }

private:
    void init_users(int n);
    void worker_loop(int thread_idx, int start_idx, int end_idx);

    moodycamel::ConcurrentQueue<Order>& queue_;
    GBMSimulator&                        market_;
    std::string                          instrument_;

    std::vector<SimUser>                             users_;
    std::unordered_map<std::string, UserAccount>     accounts_;
    std::vector<std::thread>                         threads_;
    std::atomic<bool>                                running_{false};
    double                                           total_deposited_ = 0.0;
};

} // namespace asgard::sim
