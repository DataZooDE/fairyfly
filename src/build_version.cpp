#include "include/build_version.h"
#include "include/version.h"

namespace fairyfly {

bool is_release_build() { return true; }
const char* build_commit() { return ""; }
std::string compose_display_version(std::string_view base, bool, std::string_view) { return std::string(base); }
const std::string& display_version() {
    static const std::string v = FAIRYFLY_VERSION;
    return v;
}
std::string build_description() { return std::string("fairyfly ") + FAIRYFLY_VERSION; }

}  // namespace fairyfly
