#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace fairyfly::mcp {

struct ScreenRef {
    std::string element_id;
    std::string principal;
    std::string guard;
    std::string session;
    std::string window_title;
    std::optional<int> connection;
};

/// In-memory, bounded field references shared across short-lived owner-mode providers.
/// Each lookup still checks principal, connection, session and current dynpro.
struct ScreenRefStore {
    std::mutex mutex;
    std::map<std::string, ScreenRef> refs;
};

} // namespace fairyfly::mcp
