#pragma once
// Bounded read of the SAP session facts (system, client, user, transaction) that the token allowlists and the audit
// trail depend on. The SAP GUI scripting API answers a property read only when the GUI is not busy with a server
// round trip; an unbounded read on the executor thread could stall a whole MCP call (and every call behind it).
// COM objects are bound to the main (STA) thread, so a blocked read cannot be abandoned from another thread: the
// bound is applied BEFORE touching the properties (wait for `Busy` to clear, at most `budget`) and between reads.
// When the budget is spent the answer is "no facts", which makes the token authorizer fail closed
// (SYSTEM_UNKNOWN / TCODE_DENIED) instead of hanging the call.

#include <chrono>
#include <functional>
#include <thread>

#include "include/audit_log.h"

namespace fairyfly::sap {

struct FactsBudget {
    std::chrono::milliseconds total{5000};  ///< give up after this long
    std::chrono::milliseconds poll{100};    ///< pause between Busy checks
};

/// Seams so the bound can be tested without SAP or real waiting.
struct FactsClock {
    std::function<std::chrono::steady_clock::time_point()> now = [] { return std::chrono::steady_clock::now(); };
    std::function<void(std::chrono::milliseconds)> sleep = [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
};

/// Waits (at most `budget.total`) until `is_busy()` is false, then calls `read()`. `is_busy` / `read` may throw: a throwing
/// `is_busy` (property not readable) counts as "not busy" and a throwing `read` as "no facts". Returns empty facts when the
/// session stayed busy for the whole budget (`*timed_out` is then true).
inline audit::SapFacts read_facts_bounded(const std::function<bool()>& is_busy,
                                          const std::function<audit::SapFacts()>& read,
                                          const FactsBudget& budget = {}, const FactsClock& clock = {},
                                          bool* timed_out = nullptr) noexcept {
    if (timed_out) *timed_out = false;
    try {
        const auto deadline = clock.now() + budget.total;
        const auto busy = [&] {
            try { return is_busy ? is_busy() : false; } catch (...) { return false; }
        };
        while (busy()) {
            if (clock.now() >= deadline) {
                if (timed_out) *timed_out = true;
                return {};
            }
            clock.sleep(budget.poll);
        }
        return read();
    } catch (...) {
        return {};
    }
}

} // namespace fairyfly::sap
