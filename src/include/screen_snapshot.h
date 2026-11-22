#pragma once
// A cheap picture of the screen (window, title, transaction, status text) to notice that an action changed it.
#include <string>

namespace fairyfly::sap {

struct ScreenSnapshot {
    std::string window_id;
    std::string title;
    std::string transaction;
    std::string statusbar_text;
    std::string program;         ///< GuiSessionInfo.Program (dynpro program)
    std::string screen_number;   ///< GuiSessionInfo.ScreenNumber ("" = not readable)
};

bool screen_snapshot_changed(const ScreenSnapshot& before, const ScreenSnapshot& after);

}  // namespace fairyfly::sap
