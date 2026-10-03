#pragma once

#include "core.h"
#include "cli_handler.h"
#include "com/wrapper.h"
#include <memory>
#include <vector>
#include <string>

namespace fairyfly {
namespace sap {

/// Where a capture ends up: the crop (always in NATIVE window pixels, applied BEFORE scaling) and the output size.
struct CaptureGeometry {
    int native_width = 0;
    int native_height = 0;
    bool cropped = false;
    int crop_x = 0, crop_y = 0, crop_width = 0, crop_height = 0;  ///< the crop actually used (clamped to the image)
    bool crop_clamped = false;                                    ///< the requested rectangle overhung the image
    int output_width = 0;
    int output_height = 0;
};

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

    /// Pure crop/scale math: crop in native pixels first (clamped to the image; a rectangle completely outside
    /// throws std::invalid_argument naming the native size), then the scale on the cropped size.
    static CaptureGeometry plan_capture_geometry(int native_width, int native_height,
                                                 const cli::ScreenshotOptions& opts);
    /// {native_size, crop?, crop_clamped?, output_size} for the result JSON.
    static json geometry_json(const CaptureGeometry& plan);

    /// Base64 output of a capture: without a file (or with "-") the data URI is returned in `screenshot`; with a file path
    /// the identical data URI text is written to that file and the result carries `filepath` instead (like png).
    /// Throws std::runtime_error when the file cannot be written.
    static void emit_base64_output(Result& result, const std::string& base64_data, const std::string& output_file);

    /// Validate subsection parameters (all-or-nothing)
    static void validate_subsection_complete(const cli::ScreenshotOptions& opts);

private:
    ComGuiSessionPtr session_;
};

} // namespace sap
} // namespace fairyfly

