#include "include/trace.h"

namespace fairyfly {
namespace utils {

TraceGuard::TraceGuard(const std::string& operation)
    : operation_(operation), start_(std::chrono::high_resolution_clock::now()) {
    spdlog::trace("TRACE_ENTER|{}", operation_);
}

TraceGuard::~TraceGuard() {
    log_exit();
}

void TraceGuard::log_exit() {
    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start_).count();

    if (success_) {
        spdlog::trace("TRACE_EXIT|{}|success|duration_ms={}", operation_, duration_ms);
    } else {
        spdlog::trace("TRACE_EXIT|{}|error|duration_ms={}|error={}", operation_, duration_ms, error_msg_);
    }
}

} // namespace utils
} // namespace fairyfly
