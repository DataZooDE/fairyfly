#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>
#include "include/config.h"
#include "include/session.h"

using namespace fairyfly;
using namespace fairyfly::config;
using namespace fairyfly::session;

// Note: Profile and session management have been removed in Phase 2A.
// Config and session managers are now deprecated stubs.
// Tests below verify the stub interface still compiles.

TEST_CASE("ConfigManager stub operations", "[config]") {
    ConfigManager config;

    SECTION("ConfigManager can be created") {
        // Verify stub interface compiles
        REQUIRE(config.load() == true);
        REQUIRE(config.save() == true);
        REQUIRE(config.list_profiles().empty());
        REQUIRE(!config.get("any_key"));
        config.set("any_key", "any_value");
    }
}

TEST_CASE("SessionManager stub operations", "[session]") {
    SessionManager manager;

    SECTION("SessionManager can be created") {
        // Verify stub interface compiles
        REQUIRE(manager.load_all() == true);
        REQUIRE(manager.list_sessions().empty());
        REQUIRE(manager.cleanup_expired_sessions() == 0);
    }
}
