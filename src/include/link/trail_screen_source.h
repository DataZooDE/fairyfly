#pragma once
// The audit trail's view of SAP GUI (trail_recorder.h ScreenSource) through SAP GUI Scripting: the active window of
// the session the link's calls use, its elements (one GuiSession.GetObjectTree call, a per-element walk on SAP GUI
// releases without it), the screen rectangles of the fields to blur, and HardCopyToMemory. Main (COM) thread only.
#include <functional>
#include <memory>

#include "include/link/trail_recorder.h"

namespace fairyfly {
namespace cli {
class CommandHandler;
}
namespace sap {
class ComGuiSession;
}
}  // namespace fairyfly

namespace fairyfly::link::trail {

/// The session attached in the link's command handler (`peek` returns it, or nullptr before the first call).
std::shared_ptr<ScreenSource> make_com_screen_source(std::function<cli::CommandHandler*()> peek);
/// Any session (the live check in tests/unit/test_trail_recorder.cpp).
std::shared_ptr<ScreenSource> make_com_screen_source_for(std::function<std::shared_ptr<sap::ComGuiSession>()> session);

}  // namespace fairyfly::link::trail
