#pragma once
// Sets one process environment variable for a test and puts the previous value (or its absence) back.
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>

namespace fairyfly::test {

class EnvGuard {
public:
    EnvGuard(std::string name, const std::string& value) : name_(std::move(name)) {
        if (const char* v = std::getenv(name_.c_str())) {
            had_ = true;
            old_ = v;
        }
        REQUIRE(set(value) == 0);
    }
    ~EnvGuard() { set(had_ ? old_ : std::string()); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    int set(const std::string& value) const {
#ifdef _WIN32
        return _putenv_s(name_.c_str(), value.c_str());
#else
        return value.empty() ? unsetenv(name_.c_str()) : setenv(name_.c_str(), value.c_str(), 1);
#endif
    }
    std::string name_, old_;
    bool had_ = false;
};

}  // namespace fairyfly::test
