#include "include/config/environment.h"

#include <cctype>
#include <cstdlib>
#include <utility>

namespace fairyfly::config {

namespace {
std::string read_process_variable(const char* name) {
#ifdef _WIN32
    char* buffer = nullptr;
    size_t size = 0;
    std::string value;
    if (_dupenv_s(&buffer, &size, name) == 0 && buffer) {
        value = buffer;
        std::free(buffer);
    }
    return value;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}
}  // namespace

std::string normalized(std::string_view value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.remove_prefix(1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.remove_suffix(1);
    std::string lower;
    for (char c : value) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower;
}

bool parse_flag(std::string_view value) {
    const std::string v = normalized(value);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

bool parse_off(std::string_view value) {
    const std::string v = normalized(value);
    return v == "0" || v == "false" || v == "no" || v == "off";
}

Environment::Environment(EnvLookup lookup) : lookup_(std::move(lookup)) {}

const Environment& Environment::process() {
    static const Environment instance(read_process_variable);
    return instance;
}

std::string Environment::get(const char* name) const { return lookup_ ? lookup_(name) : std::string(); }

std::filesystem::path Environment::local_app_data() const {
    const std::string dir = get(env::kLocalAppData);
    return dir.empty() ? std::filesystem::temp_directory_path() : std::filesystem::path(dir);
}

EnvLookup process_env() { return Environment::process().lookup(); }

}  // namespace fairyfly::config
