#include "include/element_type_registry.h"
#include <algorithm>

namespace fairyfly {
namespace sap {

// ============================================================================
// ElementTypeDescriptor Implementation
// ============================================================================

ElementTypeDescriptor::ElementTypeDescriptor(
    std::string type_pattern,
    bool supports_visual_props,
    bool supports_accessibility,
    std::vector<std::string> base_capabilities
)
    : type_pattern_(std::move(type_pattern))
    , supports_visual_properties_(supports_visual_props)
    , supports_accessibility_(supports_accessibility)
    , base_capabilities_(std::move(base_capabilities))
{
}

bool ElementTypeDescriptor::matches(const std::string& type) const {
    // Exact match
    if (type == type_pattern_) {
        return true;
    }

    // Substring match (e.g., "GuiTextField" matches "TextField")
    return type.find(type_pattern_) != std::string::npos;
}

std::vector<std::string> ElementTypeDescriptor::derive_capabilities(
    bool enabled,
    bool changeable
) const {
    std::vector<std::string> capabilities = base_capabilities_;

    // Filter capabilities based on element state
    if (!enabled) {
        // Remove interactive capabilities if disabled
        capabilities.erase(
            std::remove_if(capabilities.begin(), capabilities.end(),
                [](const std::string& cap) {
                    return cap == "clickable" || cap == "fillable" || cap == "selectable";
                }),
            capabilities.end()
        );
    }

    if (!changeable) {
        // Remove modification capabilities if not changeable
        capabilities.erase(
            std::remove_if(capabilities.begin(), capabilities.end(),
                [](const std::string& cap) {
                    return cap == "fillable" || cap == "selectable";
                }),
            capabilities.end()
        );
    }

    return capabilities;
}

// ============================================================================
// ElementTypeRegistry Implementation
// ============================================================================

ElementTypeRegistry::ElementTypeRegistry() {
    register_default_types();
}

ElementTypeRegistry& ElementTypeRegistry::instance() {
    static ElementTypeRegistry instance;
    return instance;
}

void ElementTypeRegistry::register_type(
    const std::string& name,
    ElementTypeDescriptor descriptor
) {
    descriptors_.emplace(name, std::move(descriptor));
}

const ElementTypeDescriptor* ElementTypeRegistry::find(const std::string& type) const {
    // Try exact match first
    auto it = descriptors_.find(type);
    if (it != descriptors_.end()) {
        return &it->second;
    }

    // Try pattern matching
    for (const auto& [name, descriptor] : descriptors_) {
        if (descriptor.matches(type)) {
            return &descriptor;
        }
    }

    return nullptr;
}

bool ElementTypeRegistry::supports_visual_properties(const std::string& type) const {
    const auto* descriptor = find(type);
    return descriptor && descriptor->supports_visual_properties();
}

bool ElementTypeRegistry::supports_accessibility(const std::string& type) const {
    const auto* descriptor = find(type);
    return descriptor && descriptor->supports_accessibility();
}

std::vector<std::string> ElementTypeRegistry::derive_capabilities(
    const std::string& type,
    bool enabled,
    bool changeable
) const {
    const auto* descriptor = find(type);
    if (!descriptor) {
        return {};  // Unknown type, no capabilities
    }

    return descriptor->derive_capabilities(enabled, changeable);
}

void ElementTypeRegistry::register_default_types() {
    // Visual interactive elements
    register_type("GuiButton", ElementTypeDescriptor{
        "Button",
        true,   // supports visual properties
        true,   // supports accessibility
        {"clickable"}
    });

    register_type("GuiTextField", ElementTypeDescriptor{
        "TextField",
        true, true,
        {"fillable", "readable"}
    });

    register_type("GuiCTextField", ElementTypeDescriptor{
        "CTextField",
        true, true,
        {"fillable", "readable"}
    });

    register_type("GuiPasswordField", ElementTypeDescriptor{
        "PasswordField",
        true, true,
        {"fillable", "readable"}
    });

    register_type("GuiOkCodeField", ElementTypeDescriptor{
        "OkCode",
        true, true,
        {"fillable", "readable"}
    });

    register_type("GuiCheckBox", ElementTypeDescriptor{
        "CheckBox",
        true, true,
        {"selectable", "readable"}
    });

    register_type("GuiRadioButton", ElementTypeDescriptor{
        "RadioButton",
        true, true,
        {"selectable", "readable"}
    });

    register_type("GuiComboBox", ElementTypeDescriptor{
        "ComboBox",
        true, true,
        {"selectable", "readable", "has_options"}
    });

    register_type("GuiComboBoxControl", ElementTypeDescriptor{
        "ComboBoxControl",
        true, true,
        {"selectable", "readable", "has_options"}
    });

    register_type("GuiLabel", ElementTypeDescriptor{
        "Label",
        true, true,
        {"readable"}
    });

    register_type("GuiTab", ElementTypeDescriptor{
        "Tab",
        true, true,
        {"clickable"}
    });

    register_type("GuiBox", ElementTypeDescriptor{
        "Box",
        true, true,
        {"container"}
    });

    // Table and grid elements
    register_type("GuiTableControl", ElementTypeDescriptor{
        "Table",
        true, true,
        {"readable", "has_rows"}
    });

    register_type("GuiGridView", ElementTypeDescriptor{
        "Grid",
        true, true,
        {"readable", "has_rows"}
    });

    register_type("GuiTree", ElementTypeDescriptor{
        "Tree",
        true, true,
        {"readable", "has_nodes"}
    });

    // Container elements (visual)
    register_type("GuiUserArea", ElementTypeDescriptor{
        "UserArea",
        true, true,
        {"container"}
    });

    register_type("GuiScrollContainer", ElementTypeDescriptor{
        "ScrollContainer",
        true, true,
        {"container"}
    });

    register_type("GuiSimpleContainer", ElementTypeDescriptor{
        "SimpleContainer",
        true, true,
        {"container"}
    });

    // Non-visual container elements (don't support visual/accessibility props)
    register_type("GuiMenubar", ElementTypeDescriptor{
        "Menubar",
        false,  // no visual properties
        false,  // no accessibility
        {"clickable"}
    });

    register_type("GuiToolbar", ElementTypeDescriptor{
        "Toolbar",
        false, false,
        {"clickable"}
    });

    register_type("GuiTitlebar", ElementTypeDescriptor{
        "Titlebar",
        false, false,
        {"readable"}
    });

    register_type("GuiStatusbar", ElementTypeDescriptor{
        "Statusbar",
        false, false,
        {"readable"}
    });

    register_type("GuiTabStrip", ElementTypeDescriptor{
        "TabStrip",
        false, false,
        {"container"}
    });

    register_type("GuiMenu", ElementTypeDescriptor{
        "Menu",
        false, false,
        {"clickable"}
    });

    register_type("GuiToolbarControl", ElementTypeDescriptor{
        "ToolbarControl",
        false, false,
        {"clickable"}
    });
}

} // namespace sap
} // namespace fairyfly
