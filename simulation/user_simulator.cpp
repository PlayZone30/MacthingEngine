#include "user_simulator.hpp"
#include <chrono>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace asgard::sim {

using namespace std::chrono;

// ---------------------------------------------------------------------------
// init_users
// ---------------------------------------------------------------------------

void UserSimulator::init_users(int n) {
    std::mt19937_64 rng(42); // deterministic seed for reproducibility
    std::lognormal_distribution<double> wallet_dist(9.0, 1.5); // ~8k median
    std::uniform_real_distribution<double> u(0.0, 1.0);

    // User type distribution:
    //   80% retail, 15% algo, 5% MM
    const int n_retail = static_cast<int>(n * 0.80);
    const int n_algo   = static_cast<int>(n * 0.15);
    const int n_mm     = n - n_retail - n_algo;

    auto make_user = [&](int idx, const UserProfile& profile) {
        SimUser su;
        su.profile = profile;

        std::ostringstream oss;
        oss << profile.name << "-" << std::setfill('0') << std::setw(5) << idx;
        su.account.user_id       = oss.str();
        su.account.user_type     = profile.type;
        su.account.rate_limit    = profile.rate_limit;

        // Sample wallet balance (clamped to profile range)
        double wb = wallet_dist(rng);
        wb = std::clamp(wb, profile.wallet_min, profile.wallet_max);
        su.account.wallet_balance  = wb;
        su.account.total_deposited = wb;
        total_deposited_ += wb;

        // Stagger initial order times so not all users fire at t=0
        su.next_order_time_s = u(rng) * (1.0 / profile.arrival_rate);

        accounts_[su.account.user_id] = su.account;
        users_.push_back(std::move(su));
    };

    for (int i = 0; i < n_retail; ++i) make_user(i, retail_profile());
    for (int i = 0; i < n_algo;   ++i) make_user(n_retail + i, algo_profile());
    for (int i = 0; i < n_mm;     ++i) make_user(n_retail + n_algo + i, mm_profile());
}

// ---------------------------------------------------------------------------
// worker_loop
// ---------------------------------------------------------------------------

void UserSimulator::worker_loop(int thread_idx, int start_idx, int end_idx) {
    OrderGenerator gen(static_cast<uint64_t>(thread_idx) * 1000 + 42);

    auto sim_start = steady_clock::now();

    while (running_.load(std::memory_order_relaxed)) {
        double now_s = duration<double>(steady_clock::now() - sim_start).count();

        bool any_due = false;
        for (int i = start_idx; i < end_idx; ++i) {
            SimUser& su = users_[i];
            if (now_s < su.next_order_time_s) continue;

            any_due = true;

            // Generate order(s) using the latest market price
            double mp = market_.price();
            // Read user account snapshot (engine thread may update it, but we
            // only READ here for order generation — actual balance checks happen
            // on the engine thread after dequeuing).
            const UserAccount& acc = accounts_.at(su.account.user_id);
            auto orders = gen.generate(acc, su.profile, instrument_, mp);

            for (auto& o : orders) {
                queue_.enqueue(o);
            }

            // Schedule next order
            double interval = gen.inter_arrival(su.profile.arrival_rate);
            su.next_order_time_s = now_s + interval;
        }

        if (!any_due) {
            // Sleep 1ms to avoid busy-waiting
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

// ---------------------------------------------------------------------------
// start / stop
// ---------------------------------------------------------------------------

void UserSimulator::start() {
    running_.store(true, std::memory_order_relaxed);

    int hw = static_cast<int>(std::thread::hardware_concurrency());
    if (hw <= 0) hw = 4;
    int nthreads = std::min(hw, static_cast<int>(users_.size()));

    int per_thread = static_cast<int>(users_.size()) / nthreads;
    for (int t = 0; t < nthreads; ++t) {
        int start = t * per_thread;
        int end   = (t == nthreads - 1) ? static_cast<int>(users_.size()) : start + per_thread;
        threads_.emplace_back([this, t, start, end]{ worker_loop(t, start, end); });
    }
}

void UserSimulator::stop() {
    running_.store(false, std::memory_order_relaxed);
    for (auto& th : threads_) {
        if (th.joinable()) th.join();
    }
    threads_.clear();
}

// ---------------------------------------------------------------------------
// account_ptrs
// ---------------------------------------------------------------------------

std::vector<UserAccount*> UserSimulator::account_ptrs() {
    std::vector<UserAccount*> ptrs;
    ptrs.reserve(accounts_.size());
    for (auto& [id, acc] : accounts_) {
        ptrs.push_back(&acc);
    }
    return ptrs;
}

} // namespace asgard::sim
