#include "include/automation_engine.h"
#include "include/com_automation_engine.h"
#include "include/cli_handler.h"
#include <spdlog/spdlog.h>
#include <chrono>

namespace fairyfly {

std::unique_ptr<AutomationEngine> AutomationEngine::create() {
    try {
        return std::make_unique<sap::ComAutomationEngine>();
    } catch (const std::exception& e) {
        spdlog::error("Failed to initialize COM engine: {}", e.what());
        throw;
    }
}

} // namespace fairyfly
