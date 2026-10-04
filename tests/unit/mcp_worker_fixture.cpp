#include <iostream>
#include <fstream>
#include <string>
#include <thread>
#include <chrono>

#include "include/mcp/session_worker_protocol.h"

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--effect-then-exit") {
        std::string line;
        if (!std::getline(std::cin, line)) return 8;
        try {
            const auto call = fairyfly::mcp::decode_worker_call(line);
            if (call.argv.size() < 2 || call.argv[0] != "element" || call.argv[1] != "fill") return 9;
            std::ofstream marker(argv[2], std::ios::binary | std::ios::app);
            if (!marker) return 10;
            marker.put('x');
            marker.flush();
            if (!marker) return 11;
            return 7;  // The side effect happened, but its response was lost.
        } catch (...) { return 12; }
    }
    if (argc > 1 && std::string(argv[1]) == "--exit-on-call") {
        std::string line;
        std::getline(std::cin, line);
        return 7;
    }
    if (argc > 1 && std::string(argv[1]) == "--hang-on-call") {
        std::string line;
        std::getline(std::cin, line);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    const bool sleep_first = argc > 1 && std::string(argv[1]) == "--sleep-first-call";
    return fairyfly::mcp::run_worker_loop(std::cin, std::cout, [sleep_first](const fairyfly::mcp::WorkerCall& call) {
        if (sleep_first && call.id == 1) std::this_thread::sleep_for(std::chrono::seconds(2));
        fairyfly::Result result;
        result.status = fairyfly::Result::Status::Success;
        result.data = {{"id", call.id}};
        if (call.probe) result.data["connection_id"] = call.connection;
        if (call.enumerate) result.data["enumerate"] = true;
        return result;
    });
}
