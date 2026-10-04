#pragma once

namespace fairyfly::mcp {

/// Internal stdio worker entrypoint. The broker starts it with private inherited
/// pipes in the tray user's interactive Windows session.
int run_session_worker_stdio();

} // namespace fairyfly::mcp
