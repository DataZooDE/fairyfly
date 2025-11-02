#pragma once

#include "core.h"
#include "cli_handler.h"
#include "com/wrapper.h"
#include <memory>
#include <vector>
#include <string>

namespace fairyfly {
namespace sap {

/// Handles screenshot capture and image processing
/// Extracted from ComAutomationEngine to improve separation of concerns
class ScreenshotHandler {
public:
    /// Constructor - takes session for accessing windows
    explicit ScreenshotHandler(ComGuiSessionPtr session);

    /// Capture screenshot with options
    Result capture(const cli::ScreenshotOptions& options);

    /// Extract SAFEARRAY bytes from VARIANT (used for HardCopyToMemory)
    static std::vector<uint8_t> extract_safearray_bytes(VARIANT& var);

    /// Parse scale parameter (float for relative, int for absolute width)
    static void parse_scale_parameter(const std::string& scale, int orig_width, int orig_height,
                                     int& new_width, int& new_height);

    /// Validate subsection parameters (all-or-nothing)
    static void validate_subsection_complete(const cli::ScreenshotOptions& opts);

private:
    ComGuiSessionPtr session_;
};

} // namespace sap
} // namespace fairyfly

