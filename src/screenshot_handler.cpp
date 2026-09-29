#include "include/screenshot_handler.h"
#include "include/com/utf8.h"
#include "include/com/raii_helpers.h"
#include "include/constants.h"
#include "include/trace.h"
#include "include/base64.h"
#include "include/string_utils.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <CImg.h>

namespace fairyfly {
namespace sap {

using utils::TraceGuard;

ScreenshotHandler::ScreenshotHandler(ComGuiSessionPtr session) : session_(session) {
}

void ScreenshotHandler::validate_subsection_complete(const cli::ScreenshotOptions& opts) {
    bool has_any = opts.crop_x.has_value() || opts.crop_y.has_value() ||
                   opts.crop_width.has_value() || opts.crop_height.has_value();
    bool has_all = opts.crop_x.has_value() && opts.crop_y.has_value() &&
                   opts.crop_width.has_value() && opts.crop_height.has_value();

    if (has_any && !has_all) {
        throw std::invalid_argument(
            "All subsection parameters (--x, --y, --width, --height) must be provided together");
    }

    if (has_all) {
        if (opts.crop_x.value() < 0 || opts.crop_y.value() < 0) {
            throw std::invalid_argument("Subsection x and y must be non-negative");
        }
        if (opts.crop_width.value() <= 0 || opts.crop_height.value() <= 0) {
            throw std::invalid_argument("Subsection width and height must be positive");
        }
    }
}

void ScreenshotHandler::parse_scale_parameter(const std::string& scale, int orig_width, int orig_height,
                                              int& new_width, int& new_height) {
    if (scale.find('.') != std::string::npos) {
        // Float: relative scale (0.5 = 50%)
        float factor = std::stof(scale);
        if (factor <= constants::MIN_SCALE_FACTOR || factor > constants::MAX_SCALE_FACTOR) {
            throw std::invalid_argument(
                fmt::format("Scale factor must be between {} and {}", constants::MIN_SCALE_FACTOR, constants::MAX_SCALE_FACTOR));
        }
        new_width = static_cast<int>(orig_width * factor);
        new_height = static_cast<int>(orig_height * factor);
    } else {
        // Integer: target width (maintain aspect ratio)
        new_width = std::stoi(scale);
        if (new_width <= 0) {
            throw std::invalid_argument("Width must be positive");
        }
        new_height = static_cast<int>(orig_height * (new_width / static_cast<float>(orig_width)));
    }
}

std::vector<uint8_t> ScreenshotHandler::extract_safearray_bytes(VARIANT& var) {
    if (var.vt != (VT_ARRAY | VT_UI1)) {
        throw std::runtime_error("Expected byte array (VT_ARRAY | VT_UI1) from HardCopyToMemory");
    }

    SAFEARRAY* psa = var.parray;
    if (!psa) {
        throw std::runtime_error("SAFEARRAY pointer is null");
    }

    // Use RAII guard for SafeArray access - guard handles locking/unlocking
    com::SafeArrayGuard guard(psa);
    if (!guard.is_locked()) {
        throw std::runtime_error("Failed to lock SafeArray for access");
    }

    // Get data pointer - SafeArrayAccessData was called by guard constructor
    BYTE* data = guard.data();
    if (!data) {
        throw std::runtime_error("SafeArray data pointer is null");
    }

    long size = guard.size();
    std::vector<uint8_t> result(data, data + size);
    // SafeArrayUnaccessData called automatically by guard destructor
    return result;
}

Result ScreenshotHandler::capture(const cli::ScreenshotOptions& options) {
    TraceGuard trace("ScreenshotHandler::capture");
    auto start = std::chrono::high_resolution_clock::now();

    Result result;

    try {
        if (!session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }

        // Validate subsection parameters
        validate_subsection_complete(options);

        // Get active window
        auto window = session_->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }

        // Get window title for filename generation
        std::string window_title = window->get_title();

        // Determine if we need CImg processing
        bool needs_processing = options.show ||
                               !options.scale.empty() ||
                               options.format == "base64" ||
                               options.output_file == "-" ||
                               (options.crop_x.has_value() && (options.format == "base64" || options.output_file == "-"));

        if (!needs_processing && !options.output_file.empty() && options.output_file != "-") {
            // Fast path: Direct HardCopy to file with subsection support
            // SAP GUI runs in another process and resolves relative HardCopy
            // paths against its own working directory, not the CLI's.
            const auto output_path = std::filesystem::absolute(
                std::filesystem::path(com::utf8_to_wide(options.output_file)));
            const std::string filename = com::wide_to_utf8(output_path.wstring());

            // Call HardCopy COM method
            VARIANT filename_var;
            VariantInit(&filename_var);
            filename_var.vt = VT_BSTR;
            const auto wide_filename = com::utf8_to_wide(filename);
            filename_var.bstrVal = SysAllocStringLen(wide_filename.data(), static_cast<UINT>(wide_filename.size()));

            VARIANT image_type;
            VariantInit(&image_type);
            image_type.vt = VT_I4;
            image_type.lVal = constants::IMAGE_TYPE_PNG;

            VARIANT result_path;
            VariantInit(&result_path);

            IDispatch* window_dispatch = window->get_dispatch();
            if (!window_dispatch) {
                result.status = Result::Status::Error;
                result.error["code"] = "INVALID_WINDOW";
                result.error["message"] = "Window COM object is invalid or has been closed";
                VariantClear(&filename_var);
                VariantClear(&image_type);
                return result;
            }

            if (options.crop_x.has_value()) {
                // HardCopy with subsection
                DISPID dispid;
                LPOLESTR method_name = const_cast<LPOLESTR>(L"HardCopy");
                HRESULT hr = window_dispatch->GetIDsOfNames(IID_NULL, &method_name, 1, LOCALE_USER_DEFAULT, &dispid);

                if (FAILED(hr)) {
                    VariantClear(&filename_var);
                    VariantClear(&image_type);
                    throw std::runtime_error(fmt::format("HardCopy method unavailable: 0x{:08X}", hr));
                }

                if (SUCCEEDED(hr)) {
                    VARIANT args[6];
                    args[5] = filename_var;
                    args[4] = image_type;
                    args[3].vt = VT_I4; args[3].lVal = options.crop_x.value();
                    args[2].vt = VT_I4; args[2].lVal = options.crop_y.value();
                    args[1].vt = VT_I4; args[1].lVal = options.crop_width.value();
                    args[0].vt = VT_I4; args[0].lVal = options.crop_height.value();

                    DISPPARAMS params;
                    params.rgvarg = args;
                    params.cArgs = 6;
                    params.rgdispidNamedArgs = nullptr;
                    params.cNamedArgs = 0;

                    hr = window_dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                                 DISPATCH_METHOD, &params, &result_path, nullptr, nullptr);

                    if (FAILED(hr)) {
                        VariantClear(&filename_var);
                        VariantClear(&image_type);
                        throw std::runtime_error(fmt::format("HardCopy failed: 0x{:08X}", hr));
                    }
                }
            } else {
                // HardCopy without subsection
                DISPID dispid;
                LPOLESTR method_name = const_cast<LPOLESTR>(L"HardCopy");
                HRESULT hr = window_dispatch->GetIDsOfNames(IID_NULL, &method_name, 1, LOCALE_USER_DEFAULT, &dispid);

                if (FAILED(hr)) {
                    VariantClear(&filename_var);
                    VariantClear(&image_type);
                    throw std::runtime_error(fmt::format("HardCopy method unavailable: 0x{:08X}", hr));
                }

                if (SUCCEEDED(hr)) {
                    VARIANT args[2];
                    args[1] = filename_var;
                    args[0] = image_type;

                    DISPPARAMS params;
                    params.rgvarg = args;
                    params.cArgs = 2;
                    params.rgdispidNamedArgs = nullptr;
                    params.cNamedArgs = 0;

                    hr = window_dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                                 DISPATCH_METHOD, &params, &result_path, nullptr, nullptr);

                    if (FAILED(hr)) {
                        VariantClear(&filename_var);
                        VariantClear(&image_type);
                        throw std::runtime_error(fmt::format("HardCopy failed: 0x{:08X}", hr));
                    }
                }
            }

            VariantClear(&filename_var);
            VariantClear(&image_type);
            VariantClear(&result_path);

            std::error_code file_error;
            if (!std::filesystem::is_regular_file(output_path, file_error) ||
                std::filesystem::file_size(output_path, file_error) == 0 || file_error) {
                result.status = Result::Status::Error;
                result.error["code"] = "SCREENSHOT_NOT_CREATED";
                result.error["message"] = "SAP GUI did not create the screenshot at the requested path";
                return result;
            }

            result.status = Result::Status::Success;
            result.data["filepath"] = filename;
            spdlog::info("Screenshot saved directly to: {}", filename);
        } else {
            // Processing path: Use HardCopyToMemory + CImg
            VARIANT image_type;
            VariantInit(&image_type);
            image_type.vt = VT_I4;
            image_type.lVal = constants::IMAGE_TYPE_PNG;

            VARIANT png_bytes_var;
            VariantInit(&png_bytes_var);

            IDispatch* window_dispatch = window->get_dispatch();
            if (!window_dispatch) {
                result.status = Result::Status::Error;
                result.error["code"] = "INVALID_WINDOW";
                result.error["message"] = "Window COM object is invalid or has been closed";
                VariantClear(&image_type);
                return result;
            }

            DISPID dispid;
            LPOLESTR method_name = const_cast<LPOLESTR>(L"HardCopyToMemory");
            HRESULT hr = window_dispatch->GetIDsOfNames(IID_NULL, &method_name, 1, LOCALE_USER_DEFAULT, &dispid);

            if (SUCCEEDED(hr)) {
                VARIANT args[1];
                args[0] = image_type;

                DISPPARAMS params;
                params.rgvarg = args;
                params.cArgs = 1;
                params.rgdispidNamedArgs = nullptr;
                params.cNamedArgs = 0;

                hr = window_dispatch->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                             DISPATCH_METHOD, &params, &png_bytes_var, nullptr, nullptr);

                if (FAILED(hr)) {
                    VariantClear(&image_type);
                    throw std::runtime_error(fmt::format("HardCopyToMemory failed: 0x{:08X}", hr));
                }
            }

            VariantClear(&image_type);

            // Extract PNG bytes
            std::vector<uint8_t> png_bytes = extract_safearray_bytes(png_bytes_var);
            VariantClear(&png_bytes_var);

            spdlog::debug("Captured {} bytes from HardCopyToMemory", png_bytes.size());

            // Load into CImg using RAII file handle
            cimg_library::CImg<unsigned char> img;
            auto tmpfile = com::create_temp_file();
            if (!tmpfile) {
                throw std::runtime_error("Failed to create temporary file for PNG loading");
            }
            std::fwrite(png_bytes.data(), 1, png_bytes.size(), tmpfile.get());
            std::rewind(tmpfile.get());
            img.load_png(tmpfile.get());
            // File automatically closed by RAII guard

            spdlog::debug("Loaded PNG into CImg: {}x{} pixels", img.width(), img.height());

            // Apply cropping if requested
            if (options.crop_x.has_value()) {
                int x1 = options.crop_x.value();
                int y1 = options.crop_y.value();
                int x2 = x1 + options.crop_width.value() - 1;
                int y2 = y1 + options.crop_height.value() - 1;

                // Validate crop bounds
                if (x1 < 0 || y1 < 0 || x2 < x1 || y2 < y1) {
                    throw std::invalid_argument(
                        fmt::format("Invalid crop bounds: ({},{}) to ({},{})", x1, y1, x2, y2));
                }
                if (x2 >= img.width() || y2 >= img.height()) {
                    throw std::invalid_argument(
                        fmt::format("Crop bounds exceed image dimensions: crop({},{}) to ({},{}) exceeds {}x{}",
                                   x1, y1, x2, y2, img.width(), img.height()));
                }

                img.crop(x1, y1, x2, y2);
                spdlog::debug("Cropped to: {}x{} pixels", img.width(), img.height());
            }

            // Apply scaling if requested
            if (!options.scale.empty()) {
                int new_width, new_height;
                parse_scale_parameter(options.scale, img.width(), img.height(), new_width, new_height);
                img.resize(new_width, new_height, -100, -100, constants::INTERPOLATION_LANCZOS);
                spdlog::debug("Scaled to: {}x{} pixels", new_width, new_height);
            }

            // Display if requested
            if (options.show) {
                std::string window_display_title = "fairyfly - Screenshot Preview";
                cimg_library::CImgDisplay display(img, window_display_title.c_str());

                spdlog::info("Displaying screenshot. Close window to continue...");

                // Wait for user to close the window
                while (!display.is_closed()) {
                    display.wait();
                }

                spdlog::info("Preview window closed");
                result.data["displayed"] = true;
            }

            // Output
            if (options.format == "base64") {
                // Save to temporary memory buffer using RAII file handle
                auto mem_tmpfile = com::create_temp_file();
                if (!mem_tmpfile) {
                    throw std::runtime_error("Failed to create temporary file for PNG encoding");
                }
                img.save_png(mem_tmpfile.get());
                std::rewind(mem_tmpfile.get());

                // Read back bytes
                std::fseek(mem_tmpfile.get(), 0, SEEK_END);
                long file_size = std::ftell(mem_tmpfile.get());
                std::rewind(mem_tmpfile.get());

                std::vector<uint8_t> output_bytes(file_size);
                std::fread(output_bytes.data(), 1, file_size, mem_tmpfile.get());
                // File automatically closed by RAII guard

                std::string base64_data = utils::base64_encode(output_bytes);
                result.data["screenshot"] = "data:image/png;base64," + base64_data;
                result.data["format"] = "base64";
                spdlog::info("Screenshot encoded as base64 ({} bytes)", base64_data.size());
            } else if (options.output_file == "-") {
                // Write to stdout
                img.save_png(stdout);
                result.data["output"] = "stdout";
                spdlog::info("Screenshot written to stdout");
            } else {
                // Save to file
                std::string filename = options.output_file.empty()
                    ? utils::generate_screenshot_filename(window_title)
                    : options.output_file;

                img.save_png(filename.c_str());
                result.data["filepath"] = filename;
                spdlog::info("Screenshot saved to: {}", filename);
            }

            result.status = Result::Status::Success;
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    } catch (const std::invalid_argument& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_ARGUMENT";
        result.error["message"] = e.what();
        spdlog::error("Invalid argument in capture_screenshot: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
        spdlog::error("Exception in capture_screenshot: {}", e.what());
    }

    return result;
}

} // namespace sap
} // namespace fairyfly

