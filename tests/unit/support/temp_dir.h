#pragma once
// A fresh, empty directory under %TEMP% for one test, removed again when the test ends (also when it fails).
#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#ifdef _WIN32
#include <process.h>
#define FAIRYFLY_TEST_PID _getpid()
#else
#include <unistd.h>
#define FAIRYFLY_TEST_PID getpid()
#endif

namespace fairyfly::test {

class TempDir {
public:
    explicit TempDir(std::string_view tag = "test") {
        static std::atomic<int> counter{0};
        path_ = std::filesystem::temp_directory_path() /
                ("fairyfly-" + std::string(tag) + "-" + std::to_string(FAIRYFLY_TEST_PID) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(counter++));
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::filesystem::path operator/(const std::filesystem::path& child) const { return path_ / child; }
    operator const std::filesystem::path&() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace fairyfly::test
