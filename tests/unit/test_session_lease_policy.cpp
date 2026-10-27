// Owner-mode identity grants and the calls that need a session lease (src/include/mcp/session_lease_policy.h).
#include <catch2/catch_test_macros.hpp>

#include "include/mcp/session_lease_policy.h"

using namespace fairyfly::mcp;

namespace {
Policy endpoint(std::vector<std::string> identities) {
    Policy p;
    p.owner_sap_identities = std::move(identities);
    return p;
}
Principal token(std::vector<std::string> identities) {
    Principal p;
    p.name = "t";
    p.sap_identities = std::move(identities);
    return p;
}
}  // namespace

TEST_CASE("owner identity: the endpoint grant intersected with the token grant", "[mcp][lease_policy]") {
    const auto one = endpoint({"S4H/100/ALICE"});
    const auto two = endpoint({"S4H/100/ALICE", "S4H/100/BOB"});
    // an unbound token keeps working on a one-identity endpoint, never on a multi-identity one
    CHECK(owner_identity_allowed(one, token({}), "S4H/100/ALICE"));
    CHECK_FALSE(owner_identity_allowed(two, token({}), "S4H/100/ALICE"));
    CHECK(owner_identity_allowed(two, token({"S4H/100/BOB"}), "S4H/100/BOB"));
    CHECK_FALSE(owner_identity_allowed(two, token({"S4H/100/BOB"}), "S4H/100/ALICE"));
    // the token cannot widen the endpoint
    CHECK_FALSE(owner_identity_allowed(one, token({"S4H/100/BOB"}), "S4H/100/BOB"));
    CHECK_FALSE(owner_identity_allowed(one, token({}), "s4h/100/alice"));  // exact match only
}

TEST_CASE("owner identity: a multi-identity endpoint needs a bound token", "[mcp][lease_policy]") {
    CHECK(owner_token_bound(endpoint({}), token({})));
    CHECK(owner_token_bound(endpoint({"S4H/100/ALICE"}), token({})));
    CHECK_FALSE(owner_token_bound(endpoint({"S4H/100/ALICE", "S4H/100/BOB"}), token({})));
    CHECK(owner_token_bound(endpoint({"S4H/100/ALICE", "S4H/100/BOB"}), token({"S4H/100/ALICE"})));
}

TEST_CASE("lease: observational reads need none, acting calls do", "[mcp][lease_policy]") {
    CHECK_FALSE(lease_required_for_call("gui_screen_find", json::object()));
    CHECK_FALSE(lease_required_for_call("gui_menu_list", json::object()));
    CHECK_FALSE(lease_required_for_call("gui_session_list", json::object()));
    CHECK(lease_required_for_call("gui_element_click", json::object()));
    CHECK(lease_required_for_call("gui_element_fill", json::object()));
    CHECK(lease_required_for_call("gui_transaction_start", json::object()));
    CHECK(lease_required_for_call("gui_session_disconnect", json::object()));
    // a read is observational unless it switches tabs
    CHECK_FALSE(lease_required_for_call("gui_element_get", json::object()));
    CHECK(lease_required_for_call("gui_element_get", json{{"activate_tab", true}}));
    CHECK(lease_required_for_call("gui_screen_read", json::object()));
    CHECK_FALSE(lease_required_for_call("gui_screen_read", json{{"no_tabs", true}}));
}
