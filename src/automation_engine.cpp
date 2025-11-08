#include "include/automation_engine.h"
#include "include/com_automation_engine.h"
#include "include/cli_handler.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace fairyfly {

// Use the real COM implementation for Windows SAP GUI
class RealAutomationEngine : public AutomationEngine {
private:
    std::unique_ptr<sap::ComAutomationEngine> impl_;

public:
    RealAutomationEngine() : impl_(std::make_unique<sap::ComAutomationEngine>()) {}

    Result attach_by_click(int timeout_seconds) override {
        return impl_->attach_by_click(timeout_seconds);
    }

    Result launch_connection(const std::string& connection_name) override {
        return impl_->launch_connection(connection_name);
    }

    Result disconnect() override {
        return impl_->disconnect();
    }

    bool is_connected() const override {
        return impl_->is_connected();
    }

    bool validate_session(const std::string& session_id) const override {
        return impl_->validate_session(session_id);
    }

    Result execute_transaction(const std::string& tcode) override {
        return impl_->execute_transaction(tcode);
    }

    Result click_element(const ElementId& element) override {
        return impl_->click_element(element);
    }

    Result fill_field(const ElementId& element, const std::string& value) override {
        return impl_->fill_field(element, value);
    }

    Result read_field(const ElementId& element) override {
        return impl_->read_field(element);
    }

    Result read_screen(bool include_structure) override {
        return impl_->read_screen(include_structure);
    }

    Result read_screen_with_tabs() override {
        return impl_->read_screen_with_tabs();
    }

    Result capture_screenshot(const cli::ScreenshotOptions& options) override {
        return impl_->capture_screenshot(options);
    }

    nlohmann::json get_application_info() const override {
        return impl_->get_application_info();
    }

    WindowId get_active_window_id() const override {
        return impl_->get_active_window_id();
    }

    ElementId resolve_element_path(const ElementId& element) const override {
        return impl_->resolve_element_path(element);
    }
};

// Legacy stub for fallback
class StubAutomationEngine : public AutomationEngine {
private:
    bool connected_ = false;

public:
    StubAutomationEngine() {
        spdlog::debug("StubAutomationEngine created");
    }

    ~StubAutomationEngine() override {
        spdlog::debug("StubAutomationEngine destroyed");
    }

    Result attach_by_click(int timeout_seconds) override {
        (void)timeout_seconds;  // Unused in stub
        auto start = std::chrono::high_resolution_clock::now();

        connected_ = true;
        auto end = std::chrono::high_resolution_clock::now();

        Result result;
        result.status = Result::Status::Success;
        result.data["window_title"] = "[Stub Window]";
        result.data["connection_id"] = "con[0]";
        result.data["session_id"] = "ses[0]";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Attached to SAP session (stub)");
        return result;
    }

    Result launch_connection(const std::string& connection_name) override {
        auto start = std::chrono::high_resolution_clock::now();

        connected_ = true;
        auto end = std::chrono::high_resolution_clock::now();

        Result result;
        result.status = Result::Status::Success;
        result.data["connection_name"] = connection_name;
        result.data["connection_id"] = "con[0]";
        result.data["session_id"] = "ses[0]";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Launched connection: {} (stub)", connection_name);
        return result;
    }

    Result disconnect() override {
        auto start = std::chrono::high_resolution_clock::now();

        connected_ = false;

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["message"] = "Disconnected successfully";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Disconnected from SAP");
        return result;
    }

    bool is_connected() const override {
        return connected_;
    }

    bool validate_session(const std::string& session_id) const override {
        (void)session_id;  // Unused in stub
        return connected_;  // Stub: always valid if connected
    }

    Result execute_transaction(const std::string& tcode) override {
        auto start = std::chrono::high_resolution_clock::now();

        if (!connected_) {
            Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "NOT_CONNECTED";
            result.error["message"] = "Not connected to SAP system";
            return result;
        }

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["tcode"] = tcode;
        result.data["message"] = "Transaction executed (stub)";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Executed transaction: {}", tcode);
        return result;
    }

    Result click_element(const ElementId& element) override {
        auto start = std::chrono::high_resolution_clock::now();

        if (!element.is_valid()) {
            Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID";
            result.error["element"] = element.path;
            return result;
        }

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["element"] = element.path;
        result.data["action"] = "click";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Clicked element: {}", element.path);
        return result;
    }

    Result fill_field(const ElementId& element, const std::string& value) override {
        auto start = std::chrono::high_resolution_clock::now();

        if (!element.is_valid()) {
            Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID";
            return result;
        }

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["element"] = element.path;
        result.data["value"] = value;
        result.data["action"] = "fill";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Filled field: {} = {}", element.path, value);
        return result;
    }

    Result read_field(const ElementId& element) override {
        auto start = std::chrono::high_resolution_clock::now();

        if (!element.is_valid()) {
            Result result;
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_ELEMENT";
            result.error["message"] = "Invalid element ID";
            return result;
        }

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["element"] = element.path;
        result.data["value"] = "[stub value]";
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Read field: {}", element.path);
        return result;
    }

    Result read_screen(bool include_structure) override {
        auto start = std::chrono::high_resolution_clock::now();

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["screen_id"] = "wnd[0]";
        result.data["title"] = "[Stub Screen]";
        if (include_structure) {
            result.data["elements"] = json::array();
        }
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Read screen structure");
        return result;
    }

    Result read_screen_with_tabs() override {
        auto start = std::chrono::high_resolution_clock::now();

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;
        result.data["screen_id"] = "wnd[0]";
        result.data["title"] = "[Stub Screen]";
        result.data["tabs_expanded"] = true;
        result.data["expanded_tab_count"] = 0;
        result.data["tabs_content"] = json::array();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Read screen with tabs (stub)");
        return result;
    }

    Result capture_screenshot(const cli::ScreenshotOptions& options) override {
        auto start = std::chrono::high_resolution_clock::now();

        auto end = std::chrono::high_resolution_clock::now();
        Result result;
        result.status = Result::Status::Success;

        if (options.format == "base64") {
            result.data["screenshot"] = "data:image/png;base64,[stub]";
            result.data["format"] = "base64";
        } else {
            result.data["filepath"] = options.output_file.empty() ? "screenshot_stub.png" : options.output_file;
        }

        if (options.show) {
            result.data["displayed"] = true;
        }

        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Captured screenshot (stub)");
        return result;
    }

    nlohmann::json get_application_info() const override {
        nlohmann::json info;
        info["connections"] = nlohmann::json::array();
        info["total_connections"] = connected_ ? 1 : 0;
        info["total_sessions"] = connected_ ? 1 : 0;
        info["note"] = "Stub implementation - no real SAP GUI data available";
        return info;
    }

    WindowId get_active_window_id() const override {
        return WindowId("wnd[0]");  // Stub always returns main window
    }

    ElementId resolve_element_path(const ElementId& element) const override {
        return element;  // Stub doesn't resolve @active, returns as-is
    }
};

std::unique_ptr<AutomationEngine> AutomationEngine::create() {
    try {
        // Try to use the real COM implementation first
        return std::make_unique<RealAutomationEngine>();
    } catch (const std::exception& e) {
        spdlog::warn("Failed to initialize COM engine, falling back to stub: {}", e.what());
        // Fall back to stub if COM initialization fails
        return std::make_unique<StubAutomationEngine>();
    }
}

} // namespace fairyfly
