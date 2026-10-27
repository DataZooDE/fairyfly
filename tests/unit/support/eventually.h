#pragma once
// Waits until a condition holds (another thread's work), instead of a fixed sleep that is too short on a slow machine.
#include <chrono>
#include <thread>

namespace fairyfly::test {

/// True as soon as `condition()` is true; false when it is still false after `within`.
template <class Condition>
bool eventually(Condition condition, std::chrono::milliseconds within = std::chrono::seconds(3)) {
    const auto end = std::chrono::steady_clock::now() + within;
    while (std::chrono::steady_clock::now() < end) {
        if (condition()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return condition();
}

}  // namespace fairyfly::test
