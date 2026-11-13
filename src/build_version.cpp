// The shown version (include/build_version.h): FAIRYFLY_VERSION for a release, <base>-dev+<short commit> otherwise.
// is_release_build() and build_commit() are in the generated build_info.cpp (src/build_info.cpp.in).
#include "include/build_version.h"
#include "include/version.h"

#include <cctype>

namespace fairyfly {

namespace {
constexpr std::size_t kShortCommit = 7;

bool hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }
}  // namespace

std::string compose_display_version(std::string_view base, bool release, std::string_view commit) {
    std::string v(base);
    if (release) return v;
    v += "-dev";
    if (commit.size() < kShortCommit) return v;
    std::string shortened;
    for (std::size_t i = 0; i < kShortCommit; ++i) {
        if (!hex(commit[i])) return v;   // not a commit id: marked, without it
        shortened += static_cast<char>(std::tolower(static_cast<unsigned char>(commit[i])));
    }
    return v + "+" + shortened;
}

const std::string& display_version() {
    static const std::string v = compose_display_version(FAIRYFLY_VERSION, is_release_build(), build_commit());
    return v;
}

std::string build_description() {
    std::string d = "fairyfly " + display_version();
    if (is_release_build()) return d + " (release)";
    const std::string commit = build_commit();
    return d + (commit.empty() ? " (development build, commit unknown)" : " (development build of commit " + commit + ")");
}

}  // namespace fairyfly
