#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

#include "include/auth/token_store.h"
#include "include/mcp/session_leases.h"
#include "include/mcp/session_policy_state.h"
#include "include/mcp/session_routing_state.h"
#include "include/mcp/session_token_status.h"

namespace fairyfly::mcp {

// One credential scan runs outside all state locks. Missing metadata only
// proves rotation/deletion when the entire store scan succeeded.
template <typename Scan>
void cleanup_invalid_token_state(SessionPolicyState& policy, SessionLeases& leases,
                                 SessionRoutingState& routing, Scan&& scan_tokens, auth::TimePoint now) {
    auto ids = policy.token_ids();
    auto lease_ids = leases.token_ids();
    auto routing_ids = routing.principal_ids();
    ids.merge(lease_ids);
    ids.merge(routing_ids);
    if (ids.empty()) return;
    auth::FreshTokenSnapshot snapshot;
    try { snapshot = scan_tokens(); } catch (...) { return; }
    for (const auto& id : ids) {
        const auto found = snapshot.by_id.find(id);
        const bool invalid = found == snapshot.by_id.end() ? snapshot.complete :
            token_definitively_invalid(found->second, now);
        if (!invalid) continue;
        leases.revoke_token(id);
        routing.revoke_principal(id);
        policy.revoke_token(id);
    }
}

class PeriodicSessionStateCleanup {
public:
    PeriodicSessionStateCleanup(std::chrono::steady_clock::duration interval, std::function<void()> sweep)
        : interval_(checked_interval(interval)), sweep_(checked_sweep(std::move(sweep))),
          thread_([this](std::stop_token stop) { run(stop); }) {}

    ~PeriodicSessionStateCleanup() {
        thread_.request_stop();
        wake_.notify_all();
    }

    PeriodicSessionStateCleanup(const PeriodicSessionStateCleanup&) = delete;
    PeriodicSessionStateCleanup& operator=(const PeriodicSessionStateCleanup&) = delete;

private:
    static std::chrono::steady_clock::duration checked_interval(std::chrono::steady_clock::duration interval) {
        if (interval <= std::chrono::steady_clock::duration::zero())
            throw std::invalid_argument("session cleanup interval must be positive");
        return interval;
    }
    static std::function<void()> checked_sweep(std::function<void()> sweep) {
        if (!sweep) throw std::invalid_argument("session cleanup callback is required");
        return sweep;
    }

    void run(std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::unique_lock<std::mutex> lock(wait_mutex_);
            wake_.wait_for(lock, interval_, [&] { return stop.stop_requested(); });
            if (stop.stop_requested()) break;
            lock.unlock();
            try { sweep_(); } catch (...) { /* transient store failures retain state */ }
        }
    }

    std::chrono::steady_clock::duration interval_;
    std::function<void()> sweep_;
    std::mutex wait_mutex_;
    std::condition_variable wake_;
    std::jthread thread_;
};

} // namespace fairyfly::mcp
