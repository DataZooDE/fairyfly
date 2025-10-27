#pragma once

#include <functional>
#include <chrono>
#include <stdexcept>
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace utils {

/// Configuration for retry behavior
struct RetryConfig {
    int max_attempts = 3;
    std::chrono::milliseconds initial_delay{100};
    std::chrono::milliseconds max_delay{5000};
    double backoff_multiplier = 2.0;

    RetryConfig() = default;
    explicit RetryConfig(int attempts) : max_attempts(attempts) {}
};

/// Execute function with exponential backoff retry
/// @param operation Function to execute, should throw on failure
/// @param config Retry configuration
/// @param operation_name Name for logging
/// @return Result of operation if successful
/// @throw Last exception if all retries exhausted
template<typename Func>
auto retry_with_backoff(Func operation, const RetryConfig& config, const std::string& operation_name)
    -> decltype(operation())
{
    using namespace std::chrono;

    std::exception_ptr last_exception;
    auto current_delay = config.initial_delay;

    for (int attempt = 1; attempt <= config.max_attempts; ++attempt) {
        try {
            spdlog::debug("retry|{}|attempt={}/{}", operation_name, attempt, config.max_attempts);
            auto result = operation();

            if (attempt > 1) {
                spdlog::info("retry|{}|success_after_attempts={}", operation_name, attempt);
            }
            return result;
        } catch (const std::exception& e) {
            last_exception = std::current_exception();
            spdlog::debug("retry|{}|attempt={}|failed|error={}", operation_name, attempt, e.what());

            if (attempt < config.max_attempts) {
                // Wait before next attempt
                auto wait_ms = duration_cast<milliseconds>(current_delay);
                spdlog::debug("retry|{}|waiting_ms={}", operation_name, wait_ms.count());
                std::this_thread::sleep_for(current_delay);

                // Increase delay for next iteration
                auto next_delay = duration_cast<milliseconds>(
                    milliseconds(static_cast<long long>(current_delay.count() * config.backoff_multiplier))
                );
                current_delay = std::min(next_delay, config.max_delay);
            }
        }
    }

    // All retries exhausted
    spdlog::error("retry|{}|exhausted_after_attempts={}", operation_name, config.max_attempts);
    std::rethrow_exception(last_exception);
}

/// Polling configuration for wait operations
struct WaitConfig {
    std::chrono::milliseconds timeout{30000};      // 30 second default
    std::chrono::milliseconds poll_interval{100};  // Check every 100ms
    std::string operation_name = "wait";

    WaitConfig() = default;
    explicit WaitConfig(std::chrono::milliseconds timeout_ms) : timeout(timeout_ms) {}
};

/// Wait for condition to become true with polling
/// @param condition Function that returns bool, true when condition met
/// @param config Wait configuration
/// @throw std::runtime_error if timeout exceeded
template<typename Func>
void wait_for(Func condition, const WaitConfig& config)
{
    using namespace std::chrono;

    auto start = high_resolution_clock::now();
    int poll_count = 0;

    while (true) {
        try {
            if (condition()) {
                auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start);
                spdlog::debug("wait|{}|success|elapsed_ms={}|polls={}",
                             config.operation_name, elapsed.count(), poll_count);
                return;
            }
        } catch (const std::exception& e) {
            spdlog::debug("wait|{}|poll_error={}", config.operation_name, e.what());
        }

        auto elapsed = duration_cast<milliseconds>(high_resolution_clock::now() - start);
        if (elapsed > config.timeout) {
            spdlog::warn("wait|{}|timeout|elapsed_ms={}|polls={}",
                        config.operation_name, elapsed.count(), poll_count);
            throw std::runtime_error("Timeout waiting for condition: " + config.operation_name);
        }

        std::this_thread::sleep_for(config.poll_interval);
        poll_count++;
    }
}

} // namespace utils
} // namespace fairyfly
