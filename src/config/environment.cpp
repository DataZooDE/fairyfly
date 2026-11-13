#include "include/config/environment.h"
#include "include/string_utils.h"
#include "include/text/utf8.h"

#include <cctype>
#include <cstdlib>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#endif
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

/// The Profile known folder: what a process started without USERPROFILE (a service's child, a stripped environment)
/// uses for the data directory instead of a temporary or relative one.
std::string known_profile_folder() {
#ifdef _WIN32
    PWSTR path = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &path)) && path) {
        try {
            out = text::wide_to_utf8(path);
        } catch (const std::exception&) {
            out.clear();
        }
    }
    if (path) CoTaskMemFree(path);
    return out;
#else
    return {};
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
    static const Environment instance([](const char* name) {
        std::string value = read_process_variable(name);
        if (value.empty() && std::string_view(name) == env::kUserProfile) value = known_profile_folder();
        return value;
    });
    return instance;
}

std::string Environment::get(const char* name) const { return lookup_ ? lookup_(name) : std::string(); }

std::filesystem::path Environment::local_app_data() const {
    const std::string dir = get(env::kLocalAppData);
    if (!dir.empty()) return text::utf8_path(dir);
    std::error_code ec;
    auto tmp = std::filesystem::temp_directory_path(ec);
    return ec ? std::filesystem::path() : tmp;
}

std::filesystem::path Environment::data_dir() const {
    if (const std::string dir = get(env::kDataDir); !dir.empty()) return text::utf8_path(dir);
    // the user profile: not virtualized for a Microsoft Store fairyfly on any Windows version (AppData is on 10)
    for (const char* home : {env::kUserProfile, env::kHome})
        if (const std::string dir = get(home); !dir.empty()) return text::utf8_path(dir) / ".fairyfly";
    // never the temp directory or a relative path: without a profile there is no data directory (fail closed)
    return {};
}

bool Environment::data_dir_overridden() const { return !get(env::kDataDir).empty(); }

std::filesystem::path Environment::legacy_data_dir() const {
    const std::string dir = get(env::kLocalAppData);
    return dir.empty() ? std::filesystem::path() : text::utf8_path(dir) / "fairyfly";
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

std::string take_from_runtime_view(const char* name) {
#ifdef _WIN32
    const std::string value = Environment::process().get(name);  // names are case-insensitive here
    const std::wstring wname = text::utf8_to_wide(name);
    std::wstring block;
    if (const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0); n > 0) {
        block.resize(n);
        const DWORD got = GetEnvironmentVariableW(wname.c_str(), block.data(), n);
        block.resize(got < n ? got : 0);
    }
    _wputenv_s(wname.c_str(), L"");                                             // the C runtime's copy and the block
    if (!block.empty()) SetEnvironmentVariableW(wname.c_str(), block.c_str());  // the block again: children inherit it
    return value;
#else
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string value = Environment::process().get(name);
    if (value.empty()) value = Environment::process().get(lower.c_str());
    unsetenv(name);
    unsetenv(lower.c_str());
    return value;
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
