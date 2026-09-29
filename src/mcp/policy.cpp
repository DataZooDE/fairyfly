#include "include/mcp/policy.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace fairyfly::mcp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool env_read_only(const EnvFn& getenv_fn) {
    std::string value;
    if (getenv_fn) {
        value = getenv_fn("FAIRYFLY_READ_ONLY");
    } else {
#ifdef _WIN32
        char* buffer = nullptr;
        std::size_t size = 0;
        if (_dupenv_s(&buffer, &size, "FAIRYFLY_READ_ONLY") == 0 && buffer != nullptr) {
            value = buffer;
            std::free(buffer);
        }
#else
        if (const char* v = std::getenv("FAIRYFLY_READ_ONLY")) value = v;
#endif
    }
    value = lower(value);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

bool is_true(const json& v) {
    if (v.is_boolean()) return v.get<bool>();
    if (v.is_string()) return lower(v.get<std::string>()) == "true";
    return false;
}

PolicyDecision refuse(const char* code, const std::string& message) {
    PolicyDecision d;
    d.allowed = false;
    d.code = code;
    d.message = message;
    return d;
}

} // namespace

PolicyDecision check_call(const ToolSpec& spec, const json& args, const Policy& policy, const EnvFn& getenv_fn) {
    const bool read_only = policy.read_only || env_read_only(getenv_fn);
    if (!read_only) return PolicyDecision{};

    if (spec.write_tool)
        return refuse("TOOL_UNAVAILABLE_READ_ONLY",
                      "The fairyfly server runs read-only; ask the user to restart it with `serve --allow-write`");

    const std::string& name = spec.def.name;
    if ((name == "sap_launch" || name == "sap_login") && args.is_object()) {
        auto it = args.find("multiple_logon");
        if (it != args.end() && it->is_string() && lower(it->get<std::string>()) == "end")
            return refuse("READ_ONLY_REFUSED",
                          "multiple_logon=end terminates other sessions and is refused while the fairyfly server "
                          "runs read-only; ask the user to restart it with `serve --allow-write`");
    }
    if (name == "sap_disconnect" && args.is_object()) {
        auto it = args.find("close_session");
        if (it != args.end() && is_true(*it))
            return refuse("READ_ONLY_REFUSED",
                          "close_session=true ends the SAP session and is refused while the fairyfly server runs "
                          "read-only; ask the user to restart it with `serve --allow-write`");
    }
    return PolicyDecision{};
}

bool tool_visible(const ToolSpec& spec, const Policy& policy) {
    return !(spec.write_tool && policy.read_only);
}

bool RateLimiter::allow(std::chrono::steady_clock::time_point now) {
    if (per_minute_ <= 0) return true;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto window = std::chrono::seconds(60);
    while (!calls_.empty() && now - calls_.front() >= window) calls_.pop_front();
    if (static_cast<int>(calls_.size()) >= per_minute_) return false;
    calls_.push_back(now);
    return true;
}

} // namespace fairyfly::mcp
