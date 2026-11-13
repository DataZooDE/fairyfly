#pragma once
// Which build this binary is: a release (built from its tag vYYYY.MM.DD[.N]) or a development build of some other
// commit. CMake decides at configure time (FAIRYFLY_BUILD_KIND, cmake/build_kind.cmake) and writes the answer into the
// generated build_info.cpp; the version files (version.h, CMakeLists.txt, vcpkg.json) keep the plain base version.
//
// A release shows FAIRYFLY_VERSION exactly (2026.11.09); any other build shows it with "-dev+<short commit>"
// (2026.11.09-dev+268d4b9), so a pilot or CI build can no longer pass for the release of the same day. The package
// versions (MSI, MSIX, the binary version fields) stay numeric: only the display string changes.
#include <string>
#include <string_view>

namespace fairyfly {

/// true when CMake found this build to be made from the tag v<FAIRYFLY_VERSION> (or was told so).
bool is_release_build();

/// The full commit id this binary was built from, "" when it was built without git.
const char* build_commit();

/// The shown version for `base`: `base` for a release, otherwise base + "-dev+" + the first 7 hex digits of
/// `commit` (lower case), or base + "-dev" when `commit` is not a commit id (empty, fewer than 7 hex digits).
std::string compose_display_version(std::string_view base, bool release, std::string_view commit);

/// What this binary shows and sends as its version: `fairyfly --version`, the trays' version line, `fairyfly doctor`,
/// the log header, MCP serverInfo, the audit records, and the link's pairing/hello version (the gateway keeps that
/// string as it is, up to 32 characters). The updater compares FAIRYFLY_VERSION (the base), not this.
const std::string& display_version();

/// The first line of a log file: "fairyfly 2026.11.09-dev+268d4b9 (development build of commit 268d4b9a04...)" or
/// "fairyfly 2026.11.09 (release)".
std::string build_description();

}  // namespace fairyfly
