#pragma once
// How this build is installed: the CMake option FAIRYFLY_PACKAGE (docs/MSIX.md).
//   msi   the per-user MSI (packaging/fairyfly.wxs, the default): fairyfly updates itself from the GitHub releases
//         (`fairyfly update apply`, the tray's background check) and "Start with Windows" is an HKCU Run value.
//   msix  a Microsoft Store package (packaging/msix/AppxManifest.xml.in): the install folder is read-only and the Store
//         updates it, so there is no self-update; the link tray starts at logon through the package's StartupTask, and
//         Run values would land in the package's private registry hive anyway.
// The decisions take the package as a value, so both are unit-tested in either build; kPackage is this build's.

// Set by CMakeLists.txt on fairyfly_core (1 for FAIRYFLY_PACKAGE=msix).
#ifndef FAIRYFLY_PACKAGE_MSIX
#define FAIRYFLY_PACKAGE_MSIX 0
#endif

namespace fairyfly::packaging {

enum class Package { Msi, Msix };

inline constexpr Package kPackage = FAIRYFLY_PACKAGE_MSIX ? Package::Msix : Package::Msi;

/// "msi" or "msix", the value of FAIRYFLY_PACKAGE.
constexpr const char* name(Package p) { return p == Package::Msix ? "msix" : "msi"; }

/// `fairyfly update check|apply` and the tray's update check and menu item: only where fairyfly may replace its own exe.
constexpr bool self_updates(Package p) { return p == Package::Msi; }

/// "Start with Windows" and --install-autostart/--remove-autostart through HKCU Run values; the MSIX package has its
/// StartupTask (Settings > Apps > Startup switches it).
constexpr bool run_key_autostart(Package p) { return p == Package::Msi; }

}  // namespace fairyfly::packaging
