// CommandHandler::handle_doctor: the preflight checks of `fairyfly doctor` (probes and judges in doctor_checks.h).
#include "include/cli_handler.h"
#include "include/build_version.h"
#include "include/doctor_checks.h"
#include "include/automation_engine.h"
#include "include/config/data_dir.h"
#include "include/link/proxy_cli.h"
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
    checks.push_back(doctor::judge_version(display_version(), is_release_build()));   // a development build: <base>-dev+<commit>
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
    {
        namespace pc = fairyfly::link::proxycfg;
        auto s = pc::process_services();
        const pc::ProbeTargets t = s.targets ? s.targets() : pc::probe_targets("", "https://fairyfly.io");
        if (!doctor_live_proxy_test_) {
            // gui_doctor (MCP) and batch: fast, and no internal proxy or PAC host names for a remote caller
            std::string state = "direct";
            if (auto resolver = pc::process_resolver()) {
                try {
                    const auto routes = resolver->routes(t.https_url);  // cached: the link has usually asked already
                    for (const auto& r : routes)
                        if (!r.direct) state = "configured";
                } catch (const std::exception&) {
                    state = "failing";
                }
            }
            checks.push_back(doctor::judge_proxy_summary(state));
        } else {
            // `fairyfly doctor` in a console: the route, its source and trust, and a real connection test (up to ~16 s
            // when the network is cut off)
            const pc::Inputs in = s.inputs ? s.inputs() : pc::Inputs{};
            doctor::ProxyFacts f;
            try {
                const pc::RouteDecision d = pc::decide(in, t.https_url, s.autoproxy);
                f.setting = pc::mode_name(d.effective.mode);
                f.setting_from = d.setting_from;
                f.locked = d.locked;
                f.route = pc::route_text(d, in.discovered_login_policy);
                f.source = d.source + (d.how.empty() ? "" : " (" + d.how + ")");
                f.windows_login = pc::windows_login(d, in.discovered_login_policy);
                f.stored_basic = pc::stored_basic_allowed(d);
                if (!d.warning.empty()) f.warnings = d.warning;
            } catch (const std::exception& e) {
                f.route = std::string("cannot be determined: ") + e.what();
            }
            for (const auto& w : in.warnings) f.warnings += (f.warnings.empty() ? "" : " ") + w;
            if (s.probe) {
                const pc::ProbeResult p = s.probe(t, std::nullopt);
                f.test_ok = p.ok;
                f.test = p.summary;
            } else {
                f.test = "not available";
            }
            checks.push_back(doctor::judge_proxy(f));
        }
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
    result.data["version"] = display_version();
    result.data["overall_health"] = doctor::overall_health(checks);
    result.data["checks"] = std::move(listed);
    return result;
}

}  // namespace cli
}  // namespace fairyfly
