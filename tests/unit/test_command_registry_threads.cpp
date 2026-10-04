#include <catch2/catch_test_macros.hpp>

#include <future>
#include <thread>

#include <CLI/CLI.hpp>

#include "include/commands/cli_app.h"
#include "include/commands/command_registry.h"

TEST_CASE("CLI command registry is isolated between worker threads", "[cli][registry][parallel]") {
    std::promise<void> a_ready;
    std::promise<void> b_ready;
    std::promise<void> a_checked;
    auto a_ready_signal = a_ready.get_future().share();
    auto b_ready_signal = b_ready.get_future().share();
    auto a_checked_signal = a_checked.get_future().share();
    bool a_still_has_list_command = false;

    std::thread a([&] {
        CLI::App app{"fairyfly"};
        fairyfly::commands::build_command_tree(app);
        const char* argv[] = {"fairyfly", "session", "list"};
        app.parse(3, argv);
        a_ready.set_value();
        b_ready_signal.wait();
        const auto& commands = fairyfly::commands::CommandRegistry::instance().all_commands();
        a_still_has_list_command = !commands.empty() && commands.front()->was_invoked();
        a_checked.set_value();
    });
    std::thread b([&] {
        a_ready_signal.wait();
        CLI::App app{"fairyfly"};
        fairyfly::commands::build_command_tree(app);
        const char* argv[] = {"fairyfly", "transaction", "start", "VA03"};
        app.parse(4, argv);
        b_ready.set_value();
        a_checked_signal.wait();
    });
    a.join();
    b.join();
    CHECK(a_still_has_list_command);
}
