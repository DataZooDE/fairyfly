#pragma once
#include "session_startup.h"
#include "tray/tray.h"

namespace fairyfly::startup {
Policy read_local(tray::TraySettings& settings, bool tray_mode, bool cli_enabled);
Policy read_local(bool tray_mode, bool cli_enabled);
Result configure_default(tray::TraySettings& settings, const std::string& connection);
} // namespace fairyfly::startup
