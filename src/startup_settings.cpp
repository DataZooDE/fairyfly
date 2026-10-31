#include "include/startup_settings.h"
#include "include/tray/tray_win32.h"

namespace fairyfly::startup {
Policy read_local(tray::TraySettings& settings, bool tray_mode, bool cli_enabled) {
    Policy result;
    result.local_enabled = tray_mode ? settings.get(tray::kAllowSapStartupSetting).value_or("0") == "1" : cli_enabled;
    result.default_connection = settings.get(tray::kSapDefaultConnectionSetting).value_or("");
    return result;
}
Policy read_local(bool tray_mode, bool cli_enabled) {
    auto settings = tray::make_win32_tray_settings();
    return read_local(*settings, tray_mode, cli_enabled);
}
Result configure_default(tray::TraySettings& settings, const std::string& connection) {
    const auto first = connection.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || connection.size() > 512 || connection.find_first_of("\r\n") != std::string::npos)
        return Result::fail("INVALID_ARGUMENT", "Specify a nonempty SAP Logon connection name (at most 512 characters).");
    const auto name = connection.substr(first, connection.find_last_not_of(" \t") - first + 1);
    if (!settings.set(tray::kSapDefaultConnectionSetting, name))
        return Result::fail("SAP_STARTUP_CONFIG_SAVE_FAILED", "Could not save the local default SAP connection. Check your Windows user settings permissions and retry.");
    return Result::ok({{"default_connection", name}});
}
} // namespace fairyfly::startup
