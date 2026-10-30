#pragma once
// Names other processes rely on: fairyfly-tray.exe (the windowless starter, its --close for the installer and the
// autostart cleanup) finds a running tray, its registry values and the notification registration through these.
// Header-only and without dependencies, because the starter is built without fairyfly_core.

namespace fairyfly::tray::contract {

/// The tray's hidden window and the registered messages another process sends it.
inline constexpr wchar_t kWindowClass[] = L"fairyfly_tray_window";
inline constexpr wchar_t kShowMenuMessage[] = L"fairyfly-tray-show-menu";
inline constexpr wchar_t kQuitMessage[] = L"fairyfly-tray-quit";

/// HKCU Run values ("Start with Windows") of the MCP tray and the link tray.
inline constexpr char kMcpRunValue[] = "fairyfly-mcp";
inline constexpr char kLinkRunValue[] = "fairyfly-link";
inline constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
/// HKCU key of the trays' small settings (TraySettings).
inline constexpr wchar_t kSettingsKey[] = L"Software\\DataZoo\\fairyfly\\tray";

/// The approval notifications: the app user model id and the COM activator the link registers (per user).
inline constexpr wchar_t kAppUserModelId[] = L"DataZoo.fairyfly";
inline constexpr wchar_t kToastActivatorClsid[] = L"{DAAF1065-EF17-4CFF-B3D2-EFAF70594959}";
inline constexpr wchar_t kAppUserModelIdKeyPrefix[] = L"Software\\Classes\\AppUserModelId\\";
inline constexpr wchar_t kClsidKeyPrefix[] = L"Software\\Classes\\CLSID\\";
/// The argument COM starts the activator's server (fairyfly-tray.exe) with: LocalServer32 appends it (MSI), the MSIX
/// manifest's com:ExeServer Arguments names it (packaging/msix/tests/Test-AppxManifest.ps1 compares them).
inline constexpr wchar_t kComServerArgument[] = L"-Embedding";
/// The MSIX package's tray application (the manifest's Application Id): <PackageFamilyName>!Tray is the AUMID of the
/// approval toasts in a Store build (include/link/toast_identity.h); it declares the toast activator.
inline constexpr wchar_t kPackageTrayApplicationId[] = L"Tray";

}  // namespace fairyfly::tray::contract
