#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <nlohmann/json.hpp>
#include "include/config/environment.h"
#include "support/fake_dispatch.h"

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

/// The whole test run gets its own profile: an in-process run_cli moves %LOCALAPPDATA%\fairyfly to the data directory
/// at its start (config/data_dir.h), and must never do that to a developer's or the CI runner's real folders.
/// FAIRYFLY_DATA_DIR, USERPROFILE, LOCALAPPDATA, HOME and XDG_CONFIG_HOME all point into a temporary folder that is
/// removed again at the end ("data dir: the unit tests never use the real profile" checks it).
class IsolatedProfile {
public:
    IsolatedProfile() {
        namespace fs = std::filesystem;
#ifdef _WIN32
        const auto pid = _getpid();
#else
        const auto pid = getpid();
#endif
        root_ = fs::temp_directory_path() / ("fairyfly-unit-" + std::to_string(pid) + "-" +
                                             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        const struct { const char* name; const char* dir; } vars[] = {
            {fairyfly::config::env::kDataDir, "profile/.fairyfly"},
            {fairyfly::config::env::kUserProfile, "profile"},
            {fairyfly::config::env::kLocalAppData, "profile/AppData/Local"},
            {fairyfly::config::env::kHome, "home"},
            {fairyfly::config::env::kXdgConfigHome, "home/.config"},
        };
        for (const auto& v : vars) {
            fs::path dir = root_ / v.dir;
            std::error_code ec;
            fs::create_directories(dir, ec);
            fairyfly::config::set_process_env(v.name, dir.make_preferred().string());
        }
    }
    ~IsolatedProfile() {
        std::error_code ec;
        std::filesystem::remove_all(root_, ec);
    }

private:
    std::filesystem::path root_;
};

}  // namespace

int main(int argc, char* argv[]) {
    // Keep unit-test CLI runs out of the real audit trail (audit tests set their own env).
    fairyfly::config::set_process_env(fairyfly::config::env::kAudit, "0");
    const IsolatedProfile profile;
#ifdef _WIN32
    HDESK hDesk = OpenDesktopA("Default", 0, FALSE, GENERIC_ALL);
    if (hDesk) {
        SetThreadDesktop(hDesk);
    }
#endif
    return Catch::Session().run(argc, argv);
}

// The DISPID cache is process-wide and keyed by SAP type: a fake GuiTextField without Changeable would otherwise
// leave a cached miss that turns the next test's read-only GuiTextField writable (order-dependent failures).
class DispatchCacheResetListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(const Catch::TestCaseInfo&) override { fairyfly::test::DispatchCacheAccess::clear_dispid_cache(); }
};
CATCH_REGISTER_LISTENER(DispatchCacheResetListener)

using json = nlohmann::json;

TEST_CASE("Basic JSON functionality", "[json]") {
    json response;
    response["status"] = "success";
    response["data"]["value"] = 42;

    REQUIRE(response["status"] == "success");
    REQUIRE(response["data"]["value"] == 42);
}

TEST_CASE("Error response structure", "[json]") {
    json response;
    response["status"] = "error";
    response["error"]["code"] = "TEST_ERROR";
    response["error"]["message"] = "Test error message";
    response["error"]["suggestions"] = json::array({"Suggestion 1", "Suggestion 2"});

    REQUIRE(response["status"] == "error");
    REQUIRE(response["error"]["code"] == "TEST_ERROR");
    REQUIRE(response["error"]["suggestions"].size() == 2);
}

