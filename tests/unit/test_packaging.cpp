// FAIRYFLY_PACKAGE (include/packaging.h, docs/MSIX.md): what a Microsoft Store (MSIX) build does differently from the MSI.
// The decisions take the package as a value, so the MSIX side is checked in the default MSI build as well; the cases
// tagged [msix-build] check this build's own wiring and run in CI's FAIRYFLY_PACKAGE=msix job.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>

#include "include/cli_handler.h"
#include "include/commands/update_command.h"
#include "include/packaging.h"
#include "include/tray/mcp_tray.h"
#include "include/tray/tray_win32.h"
#include "include/update/updater.h"
#include "include/version.h"

using namespace fairyfly;
using packaging::Package;

namespace {

Result run_update(Package package, const std::string& args) {
    CLI::App app;
    commands::UpdateCommand command(package);
    command.setup_cli(app);
    app.parse(args, false);
    cli::CommandHandler handler;
    return command.execute(handler);
}

bool has_action(const tray::TrayMenuModel& m, tray::TrayAction action) {
    for (const auto& i : m.items) {
        if (!i.separator && i.action == action) return true;
        for (const auto& c : i.children)
            if (c.action == action) return true;
    }
    return false;
}

struct RecordingRunKey : tray::RegistryRunKey {
    std::map<std::string, std::string> values;
    int writes = 0;
    bool set(const std::string& name, const std::string& command) override { writes++; values[name] = command; return true; }
    bool remove(const std::string& name) override { writes++; values.erase(name); return true; }
    std::optional<std::string> get(const std::string& name) override {
        auto it = values.find(name);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
};

}  // namespace

TEST_CASE("packaging: the MSI updates itself and uses Run values, the MSIX package does neither", "[packaging]") {
    CHECK(std::string(packaging::name(Package::Msi)) == "msi");
    CHECK(std::string(packaging::name(Package::Msix)) == "msix");
    CHECK(packaging::self_updates(Package::Msi));
    CHECK_FALSE(packaging::self_updates(Package::Msix));
    CHECK(packaging::run_key_autostart(Package::Msi));
    CHECK_FALSE(packaging::run_key_autostart(Package::Msix));
}

TEST_CASE("packaging: in an MSIX build `update check` and `update apply` point to the Store, without asking GitHub", "[packaging][update]") {
    for (const std::string args : {"update check", "update apply", "update apply --allow-unsigned"}) {
        INFO(args);
        const Result r = run_update(Package::Msix, args);
        REQUIRE(r.status == Result::Status::Error);
        CHECK(r.error.at("code") == "UPDATE_MANAGED_BY_STORE");
        CHECK(r.error.at("message").get<std::string>().find("Microsoft Store") != std::string::npos);
        CHECK(r.error.at("update_channel") == "microsoft-store");
        CHECK(r.error.at("current") == FAIRYFLY_VERSION);
        REQUIRE(r.error.at("suggestions").is_array());
        CHECK_FALSE(r.error.at("suggestions").empty());
        // the structured answer, as the CLI and the MCP tools print it
        CHECK(r.to_json().at("error").at("code") == "UPDATE_MANAGED_BY_STORE");
    }
}

TEST_CASE("packaging: the update help of an MSIX build names the Store and keeps its examples", "[packaging][update]") {
    for (Package package : {Package::Msi, Package::Msix}) {
        CLI::App app;
        commands::UpdateCommand command(package);
        command.setup_cli(app);
        for (const char* verb : {"check", "apply"}) {
            INFO(packaging::name(package) << " update " << verb);
            const std::string footer = app.get_subcommand("update")->get_subcommand(verb)->get_footer();
            CHECK(footer.find(std::string("Example: fairyfly update ") + verb) != std::string::npos);   // agent help needs it
            CHECK((footer.find("Microsoft Store") != std::string::npos) == (package == Package::Msix));
        }
    }
}

TEST_CASE("packaging: the update decision for a Store installation refuses with the Store's code", "[packaging][update]") {
    const auto d = update::store_managed();
    CHECK(d.verdict == update::Verdict::Refuse);
    CHECK(d.code == "UPDATE_MANAGED_BY_STORE");
    CHECK(d.message.find("Microsoft Store") != std::string::npos);
}

TEST_CASE("packaging: no update item in either tray of an MSIX build", "[packaging][tray]") {
    tray::TrayState state;
    CHECK(has_action(tray::build_menu({}, state), tray::TrayAction::UpdateCheck));   // the MSI default
    state.self_update = false;
    const auto mcp = tray::build_menu({}, state);
    CHECK_FALSE(has_action(mcp, tray::TrayAction::UpdateCheck));
    CHECK(has_action(mcp, tray::TrayAction::Quit));

    tray::LinkView link;
    link.paired = true;
    link.state = "connected";
    CHECK(has_action(tray::build_link_menu(link), tray::TrayAction::UpdateCheck));
    const auto linked = tray::build_link_menu(link, {}, {}, {}, false);
    CHECK_FALSE(has_action(linked, tray::TrayAction::UpdateCheck));
    CHECK(has_action(linked, tray::TrayAction::Quit));
}

TEST_CASE("packaging: the link tray controller of an MSIX build shows no update item", "[packaging][tray]") {
    tray::LinkTrayServices services;
    services.self_update = false;
    tray::LinkTrayController controller(services, {});
    CHECK_FALSE(has_action(controller.menu(), tray::TrayAction::UpdateCheck));
    services.self_update = true;
    tray::LinkTrayController msi(services, {});
    CHECK(has_action(msi.menu(), tray::TrayAction::UpdateCheck));
}

TEST_CASE("packaging: without Run-value autostart, --install-autostart and --remove-autostart write nothing and say why", "[packaging][tray][autostart]") {
    for (bool install : {true, false}) {
        RecordingRunKey run;
        tray::TrayEntryServices services;
        services.registry = &run;
        tray::TrayEntry entry;
        entry.tray_requested = true;
        entry.exe = "C:\\Program Files\\WindowsApps\\DataZoo.fairyfly_2026.10.2700.0_x64__abc\\fairyfly.exe";
        entry.install_autostart = install;
        entry.remove_autostart = !install;
        entry.run_key_autostart = false;
        const auto r = tray::decide_tray_entry(entry, services);
        CHECK(r.outcome == tray::TrayEntryOutcome::Failed);
        CHECK(r.exit_code != 0);
        CHECK(r.message.find("TRAY_AUTOSTART_PACKAGED") != std::string::npos);
        CHECK(r.message.find("Startup") != std::string::npos);
        CHECK(run.writes == 0);
    }
}

TEST_CASE("packaging: this build's trays and updater follow FAIRYFLY_PACKAGE", "[packaging][msix-build]") {
    // the tray entry carries the build's autostart mechanism (prepare_tray_start creates no mutex, key or process)
    CHECK(tray::prepare_tray_start("Local\\fairyfly-unit-test-not-acquired").entry.run_key_autostart ==
          packaging::run_key_autostart(packaging::kPackage));
    if (packaging::self_updates(packaging::kPackage)) SKIP("an MSI build: update::apply is covered in test_update.cpp");
    // a Store build never downloads or swaps fairyfly.exe, whatever the channel says: no server listens on this URL
    const auto a = update::apply("2000.01.01", true, "https://127.0.0.1:1/never-asked");
    CHECK(a.decision.verdict == update::Verdict::Refuse);
    CHECK(a.decision.code == "UPDATE_MANAGED_BY_STORE");
    CHECK(a.installed_path.empty());
}
