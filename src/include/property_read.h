#pragma once
// Result of a STRICT COM property read: unlike SapGuiObject::get_string_property (which turns every failure into ""),
// the status says whether the value is real. Used where an unreadable value must fail closed (selection input).

#include <string>

namespace fairyfly::sap {

enum class PropertyStatus {
    Ok,            ///< the property exists and was read (value may legitimately be empty)
    NotSupported,  ///< the control type has no such property / no such sub-object (GetIDsOfNames miss, or Nothing)
    Failed         ///< the property exists but reading it failed (COM error, unexpected variant type, busy session)
};

struct PropertyRead {
    PropertyStatus status = PropertyStatus::Failed;
    std::string value;
};

}  // namespace fairyfly::sap
