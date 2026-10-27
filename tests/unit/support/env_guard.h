#pragma once
// Sets one process environment variable for a test and puts the previous value (or its absence) back. Values are
// UTF-8, written and read through config (the wide environment), as fairyfly itself does.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "include/config/environment.h"

namespace fairyfly::test {

class EnvGuard {
public:
    EnvGuard(std::string name, const std::string& value)
        : name_(std::move(name)), old_(config::Environment::process().get(name_.c_str())) {
        set(value);
        REQUIRE(config::Environment::process().get(name_.c_str()) == value);
    }
    ~EnvGuard() { set(old_); }
    EnvGuard(const EnvGuard&) = delete;
    EnvGuard& operator=(const EnvGuard&) = delete;

private:
    void set(const std::string& value) const {
        if (value.empty()) config::unset_process_env(name_.c_str());  // on Windows an empty value is no variable
        else config::set_process_env(name_.c_str(), value);
    }
    std::string name_, old_;
};

}  // namespace fairyfly::test
