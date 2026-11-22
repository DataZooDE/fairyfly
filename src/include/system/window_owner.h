#pragma once

#include <cstdint>

namespace fairyfly::system {

// SAP GuiFrameWindow.Handle is a signed COM Long. Windows sign-extends
// 32-bit user handles when a 64-bit process receives them.
constexpr std::uintptr_t sap_com_long_to_window_handle(std::int32_t value) noexcept {
    return static_cast<std::uintptr_t>(static_cast<std::intptr_t>(value));
}

// Fail closed when the process/window cannot be verified. Intended for the
// owner-routed MCP path, where SAP GUI must belong to the tray's logon.
bool process_owned_by_current_logon(std::uint32_t process_id) noexcept;
/// The same check on a process handle the caller already holds (PROCESS_QUERY_LIMITED_INFORMATION), so the answer
/// belongs to that very process even if its id is reused later.
bool process_handle_owned_by_current_logon(void* process_handle, std::uint32_t process_id) noexcept;
bool window_owned_by_current_logon(std::uintptr_t native_handle) noexcept;

} // namespace fairyfly::system
