#include <chrono>
#include <algorithm>
#include <cctype>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "include/com/wrapper.h"
#include "include/com_automation_engine.h"
#include "include/system/window_owner.h"

namespace {
using Clock = std::chrono::steady_clock;
using json = nlohmann::json;

long long elapsed_ms(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

struct Target {
    int connection;
    int session;
};

json read_session(Target target, int iterations) {
    const auto start = Clock::now();
    auto app = fairyfly::sap::ComGuiApplication::create();
    if (target.connection < 0 || target.connection >= app->get_connection_count())
        throw std::runtime_error("requested SAP connection index is unavailable");
    auto connection = app->get_connection(target.connection);
    if (!connection || target.session < 0 || target.session >= connection->get_session_count())
        throw std::runtime_error("requested SAP session index is unavailable");
    auto session = connection->get_session(target.session);
    if (!session) throw std::runtime_error("requested SAP session is unavailable");
    const std::string initial_id = session->get_id();
    const long long setup = elapsed_ms(start);
    const auto reads_start = Clock::now();
    for (int i = 0; i < iterations; ++i) {
        if (!session->is_alive()) throw std::runtime_error("SAP session closed during probe");
        (void)session->get_transaction_code();
        (void)session->get_system_name();
        (void)session->get_client();
        (void)session->get_user();
        auto window = session->get_active_window();
        if (!window) throw std::runtime_error("SAP session has no active window");
        (void)window->get_title();
    }
    if (session->get_id() != initial_id) throw std::runtime_error("SAP session changed during probe");
    return {{"connection_index", target.connection}, {"session_index", target.session},
            {"iterations", iterations}, {"setup_ms", setup},
            {"reads_ms", elapsed_ms(reads_start)}, {"total_ms", elapsed_ms(start)}};
}

json parallel_pair(Target first, Target second, int iterations) {
    json a, b;
    std::exception_ptr error_a, error_b;
    std::promise<void> go;
    auto gate = go.get_future().share();
    std::thread ta([&] {
        gate.wait();
        try { a = read_session(first, iterations); } catch (...) { error_a = std::current_exception(); }
    });
    std::thread tb([&] {
        gate.wait();
        try { b = read_session(second, iterations); } catch (...) { error_b = std::current_exception(); }
    });
    const auto start = Clock::now();
    go.set_value();
    ta.join();
    tb.join();
    if (error_a) std::rethrow_exception(error_a);
    if (error_b) std::rethrow_exception(error_b);
    return {{"wall_ms", elapsed_ms(start)}, {"workers", json::array({a, b})}};
}

json server_wait_pair(Target slow, Target reader) {
    const json baseline = read_session(reader, 20);
    std::promise<void> started;
    auto signal = started.get_future();
    std::exception_ptr error;
    const auto origin = Clock::now();
    long long slow_started_ms = 0;
    long long slow_finished_ms = 0;
    std::thread worker([&] {
        try {
            auto app = fairyfly::sap::ComGuiApplication::create();
            if (slow.connection >= app->get_connection_count())
                throw std::runtime_error("slow SAP connection is unavailable");
            auto connection = app->get_connection(slow.connection);
            if (!connection || slow.session >= connection->get_session_count())
                throw std::runtime_error("slow SAP session is unavailable");
            auto session = connection->get_session(slow.session);
            if (!session) throw std::runtime_error("slow SAP session is unavailable");
            auto window = session->get_active_window();
            if (!window) throw std::runtime_error("slow session has no active window");
            if (session->get_transaction_code() != "SM37" || window->get_title() != "Simple Job Selection")
                throw std::runtime_error("slow target must show the prepared SM37 Simple Job Selection screen");
            const std::string base = session->get_id() + "/wnd[0]/usr/";
            auto field = [&](const char* name) {
                auto control = session->find_element_by_id(base + name);
                if (!control) throw std::runtime_error("SM37 selection field is unavailable");
                std::string value = control->get_text();
                value.erase(value.begin(), std::find_if(value.begin(), value.end(), [](unsigned char c) { return !std::isspace(c); }));
                value.erase(std::find_if(value.rbegin(), value.rend(), [](unsigned char c) { return !std::isspace(c); }).base(), value.end());
                return value;
            };
            const std::string job = field("txtBTCH2170-JOBNAME");
            const std::string user = field("txtBTCH2170-USERNAME");
            const std::string from = field("ctxtBTCH2170-FROM_DATE");
            const std::string to = field("ctxtBTCH2170-TO_DATE");
            if (job.empty() || job.find('*') != std::string::npos || user.empty() || user.find('*') != std::string::npos ||
                from.empty() || from != to)
                throw std::runtime_error("SM37 probe requires an exact job name and user, and one date only");
            slow_started_ms = elapsed_ms(origin);
            started.set_value();
            window->send_vkey(8); // F8 on a prepared, read-only SM37 job selection screen.
            slow_finished_ms = elapsed_ms(origin);
        } catch (...) {
            error = std::current_exception();
            try { started.set_value(); } catch (...) {}
        }
    });
    signal.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const long long reader_started_ms = elapsed_ms(origin);
    json live_reader;
    try { live_reader = read_session(reader, 20); }
    catch (...) { worker.join(); throw; }
    const long long reader_finished_ms = elapsed_ms(origin);
    worker.join();
    if (error) std::rethrow_exception(error);
    return {{"operation", "F8 on prepared read-only SM37 selection in A; 20 metadata reads in B"},
            {"baseline_reader", baseline}, {"slow_started_ms", slow_started_ms},
            {"f8_returned_ms", slow_finished_ms}, {"reader_started_ms", reader_started_ms},
            {"reader_finished_ms", reader_finished_ms},
            {"reader_completed_before_f8_return", reader_finished_ms < slow_finished_ms},
            {"live_reader", live_reader}};
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--facts") {
            auto app = fairyfly::sap::ComGuiApplication::create();
            auto connection = app->get_connection(std::stoi(argv[2]));
            auto session = connection->get_session(std::stoi(argv[3]));
            std::cout << json{{"session", session->get_id()}, {"description", connection->get_description()},
                              {"system", session->get_system_name()}, {"client", session->get_client()},
                              {"user", session->get_user()}, {"transaction", session->get_transaction_code()}}
                             .dump(2) << '\n';
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--window-handle") {
            auto app = fairyfly::sap::ComGuiApplication::create();
            auto connection = app->get_connection(0);
            auto session = connection->get_session(std::stoi(argv[2]));
            auto window = session->get_active_window();
            const auto raw = window->get_int_property(L"Handle");
            const auto handle = fairyfly::system::sap_com_long_to_window_handle(raw);
            fairyfly::sap::ComAutomationEngine guarded;
            guarded.set_owner_window_guard(true);
            const auto facts = guarded.peek_session_facts(session->get_id());
            std::cout << json{{"window_id", window->get_id()}, {"title", window->get_title()},
                              {"handle", raw},
                              {"owned_by_current_logon", fairyfly::system::window_owned_by_current_logon(handle)},
                              {"guarded_facts", {{"system", facts.system}, {"client", facts.client},
                                                 {"user", facts.user}}}}.dump(2) << '\n';
            return 0;
        }
        if (argc == 6 && std::string(argv[1]) == "--server-wait") {
            const Target slow{std::stoi(argv[2]), std::stoi(argv[3])};
            const Target reader{std::stoi(argv[4]), std::stoi(argv[5])};
            if (slow.connection < 0 || slow.session < 0 || reader.connection < 0 || reader.session < 0 ||
                (slow.connection == reader.connection && slow.session == reader.session))
                throw std::runtime_error("server-wait needs two distinct nonnegative connection/session targets");
            std::cout << server_wait_pair(slow, reader).dump(2) << '\n';
            return 0;
        }
        if (argc != 4 && argc != 6)
            throw std::runtime_error("usage: sap_sta_parallel_probe [CONNECTION_A] SESSION_A [CONNECTION_B] SESSION_B ITERATIONS");
        const Target first = argc == 4 ? Target{0, std::stoi(argv[1])}
                                       : Target{std::stoi(argv[1]), std::stoi(argv[2])};
        const Target second = argc == 4 ? Target{0, std::stoi(argv[2])}
                                        : Target{std::stoi(argv[3]), std::stoi(argv[4])};
        const int iterations = std::stoi(argv[argc - 1]);
        if (first.connection < 0 || first.session < 0 || second.connection < 0 || second.session < 0 ||
            (first.connection == second.connection && first.session == second.session) || iterations < 1)
            throw std::runtime_error("provide two different nonnegative connection/session targets and positive iterations");
        const auto serial_start = Clock::now();
        const json serial_a = read_session(first, iterations);
        const json serial_b = read_session(second, iterations);
        const long long serial_wall = elapsed_ms(serial_start);
        const json different = parallel_pair(first, second, iterations);
        const json same = parallel_pair(first, first, iterations);
        std::cout << json{{"model", "independent STA threads in one process"},
                          {"operation", "read-only SAP session metadata and active window title"},
                          {"serial", {{"wall_ms", serial_wall}, {"workers", json::array({serial_a, serial_b})}}},
                          {"different_sessions", different}, {"same_session", same}}.dump(2) << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "sap_sta_parallel_probe: " << error.what() << '\n';
        return 1;
    }
}
