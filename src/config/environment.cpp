#include "include/config/environment.h"
#include "include/string_utils.h"
#include "include/text/utf8.h"

#include <cstdlib>
#include <system_error>
#include <utility>

namespace fairyfly::config {

namespace {
std::string read_process_variable(const char* name) {
#ifdef _WIN32
    // wide, so a value outside the ANSI code page (a user profile path with an umlaut) arrives as UTF-8
    wchar_t* buffer = nullptr;
    size_t size = 0;
    std::string value;
    try {
        if (_wdupenv_s(&buffer, &size, text::utf8_to_wide(name).c_str()) == 0 && buffer)
            value = text::wide_to_utf8(buffer);
    } catch (const std::exception&) {
        value.clear();  // not valid UTF-16: unset rather than a garbled value
    }
    std::free(buffer);
    return value;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

}  // namespace

std::string normalized(std::string_view value) { return utils::lower_ascii(utils::trim(value)); }

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
    if (!dir.empty()) return dir;
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::path() : tmp;
}

EnvLookup process_env() { return Environment::process().lookup(); }

void set_process_env(const char* name, const std::string& value) {
#ifdef _WIN32
    // _wputenv_s updates the C runtime's copy (read above) and the process environment children inherit
    _wputenv_s(text::utf8_to_wide(name).c_str(), text::utf8_to_wide(value).c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

void unset_process_env(const char* name) {
#ifdef _WIN32
    _wputenv_s(text::utf8_to_wide(name).c_str(), L"");  // an empty value removes the variable
#else
    unsetenv(name);
#endif
}

}  // namespace fairyfly::config
