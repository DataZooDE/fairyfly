// CommandHandler::handle_doctor: the preflight checks of `fairyfly doctor` (probes and judges in doctor_checks.h).
#include "include/cli_handler.h"
#include "include/doctor_checks.h"
#include "include/automation_engine.h"
#include "include/config/data_dir.h"
#include "include/text/utf8.h"
#ifdef _WIN32
#include "include/link/desktop_approval_win32.h"
#endif

#include <spdlog/spdlog.h>

namespace fairyfly {
namespace cli {

Result CommandHandler::handle_doctor()
{
    spdlog::info("Running preflight environment and scripting diagnostics");
    std::vector<doctor::Check> checks;
#ifdef _WIN32
    std::string station, desktop;
    doctor::probe_desktop(station, desktop);
    checks.push_back(doctor::judge_desktop(station, desktop));
    checks.push_back(doctor::judge_processes(doctor::probe_processes()));
    checks.push_back(doctor::judge_client_security(doctor::probe_client_security()));
    checks.push_back(doctor::judge_notifications(packaging::kPackage, text::wide_to_utf8_lossy(link::approval::this_process_toast_identity().app_id)));
#else
    checks.push_back({"desktop_context", "info", "Non-Windows environment, desktop check skipped"});
    checks.push_back({"sapgui_processes", "info", "Non-Windows environment, process check skipped"});
    checks.push_back({"client_registry", "info", "Non-Windows environment, registry check skipped"});
#endif
    {
        const auto data = config::Environment::process().data_dir();
        const auto migration = config::last_migration();
        checks.push_back(doctor::judge_data_dir(text::path_utf8(data), migration && !migration->ok() ? migration->error : std::string(),
                                                config::recorded_conflicts(data)));
    }
    refresh_sap_gui();  // what SAP Logon is running now (no second lookup inside an MCP call that did it)
    const EngineStatus engine = engine_ ? engine_->engine_status() : EngineStatus{false, true, "no automation engine"};
    checks.push_back(doctor::judge_engine(engine.available, engine.sap_logon_running, engine.detail));
    if (engine_ && engine.available) {
        try {
            checks.push_back(doctor::judge_sessions(engine_->get_application_info()));
        } catch (const std::exception& e) {
            checks.push_back(doctor::judge_sessions_error(e.what()));
        }
    }

    json listed = json::array();
    for (const auto& c : checks) listed.push_back(doctor::to_json(c));
    Result result = Result::ok(json::object());
    result.data["overall_health"] = doctor::overall_health(checks);
    result.data["checks"] = std::move(listed);
    return result;
}

}  // namespace cli
}  // namespace fairyfly
