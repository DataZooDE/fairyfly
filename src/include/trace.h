#pragma once

#include <spdlog/spdlog.h>
#include <chrono>
#include <string>
#include <memory>
#include <cstdio>

namespace fairyfly {
namespace utils {

/// RAII guard for structured tracing with automatic entry/exit logging
/// Usage:
///   {
///       TraceGuard trace("element.click", {{"path", element.path}, {"result", "pending"}});
///       // ... do work ...
///   } // automatically logs exit with duration
class TraceGuard {
private:
    std::string operation_;
    std::chrono::high_resolution_clock::time_point start_;
    bool success_ = true;
    std::string error_msg_;

    void log_exit();

public:
    /// Create trace guard for an operation
    /// @param operation Human-readable operation name (e.g., "com.find_element")
    explicit TraceGuard(const std::string& operation);

    ~TraceGuard();

    // Disable copying
    TraceGuard(const TraceGuard&) = delete;
    TraceGuard& operator=(const TraceGuard&) = delete;

    // Allow moving
    TraceGuard(TraceGuard&&) = default;
    TraceGuard& operator=(TraceGuard&&) = default;

    /// Mark operation as successful
    void mark_success() { success_ = true; }

    /// Mark operation as failed with error message
    void mark_error(const std::string& error) {
        success_ = false;
        error_msg_ = error;
    }

    /// Add trace data to be logged
    template<typename T>
    void trace(const std::string& key, const T& value) {
        spdlog::debug("trace|{}|{}={}", operation_, key, value);
    }
};

/// Helper to trace COM property access with return value
inline std::string trace_string_result(const std::string& property, const std::string& result) {
    spdlog::debug("com_property|string|{}={}", property, result);
    return result;
}

/// Helper to trace COM property access with int return value
inline int trace_int_result(const std::string& property, int result) {
    spdlog::debug("com_property|int|{}={}", property, result);
    return result;
}

/// Helper to trace COM property access with bool return value
inline bool trace_bool_result(const std::string& property, bool result) {
    spdlog::debug("com_property|bool|{}={}", property, (result ? "true" : "false"));
    return result;
}

/// Format bytes into readable output
inline std::string format_bytes(const void* ptr) {
    if (!ptr) return "null";
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%p", ptr);
    return buffer;
}

} // namespace utils
} // namespace fairyfly
