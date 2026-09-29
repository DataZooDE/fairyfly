#include "include/connection_manager.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const std::filesystem::path directory(argv[1]);
        const int index = std::stoi(argv[2]);
        {
            std::ofstream ready(directory / ("ready." + std::to_string(index)));
            if (!ready) return 3;
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!std::filesystem::exists(directory / "start.signal")) {
            if (std::chrono::steady_clock::now() >= deadline) return 4;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        fairyfly::cli::ConnectionManager manager(directory.string());
        manager.create_or_update_connection(
            "/app/con[" + std::to_string(index) + "]/ses[0]",
            "/app/con[" + std::to_string(index) + "]",
            "Test System", "route", "SAP", "session:" + std::to_string(index));
        return 0;
    } catch (...) {
        return 5;
    }
}
