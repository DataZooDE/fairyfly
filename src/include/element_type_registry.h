#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <nlohmann/json.hpp>

namespace fairyfly {
namespace sap {

using json = nlohmann::json;

/// Describes behavioral properties and capabilities of SAP GUI element types
class ElementTypeDescriptor {
private:
    std::string type_pattern_;
    bool supports_visual_properties_;
    bool supports_accessibility_;
    std::vector<std::string> base_capabilities_;

public:
    ElementTypeDescriptor(
        std::string type_pattern,
        bool supports_visual_props,
        bool supports_accessibility,
        std::vector<std::string> base_capabilities
    );

    /// Check if this descriptor matches the given type string
    bool matches(const std::string& type) const;

    /// Check if this element type supports visual properties (Enabled, Visible, etc.)
    bool supports_visual_properties() const { return supports_visual_properties_; }

    /// Check if this element type supports accessibility properties (AccLabel, AccTooltip)
    bool supports_accessibility() const { return supports_accessibility_; }

    /// Derive runtime capabilities based on element state
    std::vector<std::string> derive_capabilities(bool enabled, bool changeable) const;

    /// Get the type pattern for debugging
    const std::string& type_pattern() const { return type_pattern_; }
};

/// Registry for SAP GUI element type metadata
/// Singleton pattern - provides centralized element type configuration
class ElementTypeRegistry {
private:
    std::unordered_map<std::string, ElementTypeDescriptor> descriptors_;

    ElementTypeRegistry();  // Private constructor for singleton

public:
    /// Get singleton instance
    static ElementTypeRegistry& instance();

    /// Register a new element type descriptor
    void register_type(const std::string& name, ElementTypeDescriptor descriptor);

    /// Find descriptor for given element type string
    /// Returns nullptr if no matching descriptor found
    const ElementTypeDescriptor* find(const std::string& type) const;

    /// Check if element type supports visual properties
    bool supports_visual_properties(const std::string& type) const;

    /// Check if element type supports accessibility properties
    bool supports_accessibility(const std::string& type) const;

    /// Derive capabilities for element with given type and state
    std::vector<std::string> derive_capabilities(
        const std::string& type,
        bool enabled,
        bool changeable
    ) const;

private:
    /// Initialize default SAP GUI element types
    void register_default_types();
};

// Helper functions for element metadata extraction

/// Check if element type is a non-visual container
/// (containers that don't support accessibility properties)
inline bool is_non_visual_container(const std::string& type) {
    return !ElementTypeRegistry::instance().supports_accessibility(type);
}

/// Derive element capabilities and return as JSON array
inline json derive_capabilities(const std::string& type, bool enabled, bool changeable) {
    auto caps = ElementTypeRegistry::instance().derive_capabilities(type, enabled, changeable);

    json capabilities = json::array();
    for (const auto& cap : caps) {
        capabilities.push_back(cap);
    }

    return capabilities;
}

} // namespace sap
} // namespace fairyfly
