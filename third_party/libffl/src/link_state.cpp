#include "link_state.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#else
#include <sys/stat.h>
#endif

#include "util.hpp"

namespace ffy::link {

namespace fs = std::filesystem;

std::string default_state_path() {
#ifdef _WIN32
    std::string base = env_or("APPDATA", ".");
#else
    std::string base = env_or("XDG_CONFIG_HOME");
    if (base.empty()) base = env_or("HOME", ".") + "/.config";
#endif
    return (fs::path(base) / "fairyfly-link" / "state.json").string();
}

#ifdef _WIN32
std::string protect(const std::string& plain) {
    DATA_BLOB in{DWORD(plain.size()), reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))}, out{};
    if (!CryptProtectData(&in, L"fairyfly-link", nullptr, nullptr, nullptr, 0, &out)) throw Error("DPAPI protect failed");
    std::string r(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return r;
}
std::string unprotect(const std::string& blob) {
    DATA_BLOB in{DWORD(blob.size()), reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))}, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, 0, &out)) throw Error("DPAPI unprotect failed (different Windows user?)");
    std::string r(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return r;
}
#else
std::string protect(const std::string& plain) { return plain; }
std::string unprotect(const std::string& blob) { return blob; }
#endif

void save_state(const State& s, const std::string& path) {
    if (!s.key) throw Error("no device key");
    nlohmann::json j = {{"link_url", s.link_url}, {"device_id", s.device_id}, {"issuer", s.issuer}, {"tenant", s.tenant}, {"email", s.email}, {"tenant_name", s.tenant_name},
                        {"key_blob", b64std_encode(protect(ec_private_pem(s.key)))}};
    j["gateway_keys"] = nlohmann::json::array();
    for (auto& k : s.gateway_keys) j["gateway_keys"].push_back(k.to_json());
    fs::create_directories(fs::path(path).parent_path());
    {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) throw Error("cannot write " + path);
        f << j.dump(2);
    }
#ifndef _WIN32
    ::chmod(path.c_str(), 0600);
#endif
}

State load_state(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Error("not enrolled (cannot read " + path + ")");
    std::stringstream ss;
    ss << f.rdbuf();
    auto j = nlohmann::json::parse(ss.str(), nullptr, false);
    if (!j.is_object()) throw Error("corrupt state file " + path);
    State s;
    auto str = [&](const char* k) { return j.value(k, std::string()); };
    s.link_url = str("link_url"); s.device_id = str("device_id"); s.issuer = str("issuer"); s.tenant = str("tenant"); s.email = str("email"); s.tenant_name = str("tenant_name");
    for (auto& k : j.value("gateway_keys", nlohmann::json::array())) s.gateway_keys.push_back(Jwk::from_json(k));
    s.key = ec_private_from_pem(unprotect(b64std_decode(str("key_blob"))));
    if (s.device_id.empty() || s.link_url.empty() || s.gateway_keys.empty()) throw Error("incomplete state file " + path);
    return s;
}

}  // namespace ffy::link
