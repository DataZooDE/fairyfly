// Desktop side of QR pairing (plan 12.1): create a key, start a pairing, show QR/code/number, collect the result.
#pragma once
#include <iosfwd>
#include <optional>
#include <string>

#include "link_state.hpp"

namespace ffy::link {

struct PairingDisplay {
    std::string link;   // what the QR encodes: https://app.../p#<pid>.<secret>
    std::string code;   // typed fallback, e.g. "QX7-K9M"
    std::string match;  // two digits the user picks on the phone
    int expires_in = 0;
};

class PairingClient {
public:
    PairingClient(std::string web_base, std::string host, std::string os, std::string version);
    PairingDisplay start();           // throws Error
    std::optional<State> poll();      // nullopt while pending (or when gone); the State once approved (single use)
    std::string status() const { return status_; }  // pending | approved | expired | denied
    bool collect_as(const std::string& pid, const PKey& key);  // test helper: collection with an arbitrary key
    // Polls until approved, expired or the deadline; returns the state on success.
    std::optional<State> wait(int timeout_s, int interval_ms = 2000);

private:
    std::optional<State> collect(const std::string& pid, const PKey& key);
    std::string base_, host_, os_, version_, pid_, status_ = "new";
    PKey key_;
};

// Shows the person at the desktop which account and organisation the pairing gives control to; true only on "yes".
// A code read over someone's shoulder can be approved into a stranger's organisation: this is the last check.
bool confirm_pairing(const State& st, std::istream& in, std::ostream& out);

}  // namespace ffy::link
