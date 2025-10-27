#pragma once

#include "core.h"
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace fairyfly {
namespace config {

/// Configuration manager - deprecated, use direct SAP attachment instead
class ConfigManager {
public:
    /// Create config manager with default paths
    ConfigManager() {}

    /// Create config manager with custom config directory
    explicit ConfigManager(const std::filesystem::path& config_dir) {}

    /// Load configuration from disk (no-op)
    bool load() { return true; }

    /// Save current configuration to disk (no-op)
    bool save() { return true; }

    /// List all available profiles (empty)
    std::vector<std::string> list_profiles() const { return {}; }

    /// Get single config value
    std::optional<std::string> get(const std::string& key) const { return {}; }

    /// Set single config value
    void set(const std::string& key, const std::string& value) {}

    /// Get config directory path
    std::filesystem::path get_config_dir() const { return std::filesystem::path(); }

    /// Get config file path
    std::filesystem::path get_config_file() const { return std::filesystem::path(); }
};

} // namespace config
} // namespace fairyfly
