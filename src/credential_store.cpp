#include "include/credential_store.h"
#include "include/cred_raii.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace fairyfly::cred {

namespace {

constexpr std::wstring_view kTargetPrefixW = L"fairyfly:";
constexpr size_t kMaxConnectionName = 200;
constexpr size_t kMaxAttributeBytes = 256;

std::wstring widen(std::string_view in) {
    if (in.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view in) {
    if (in.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, in.data(), static_cast<int>(in.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string filetime_to_iso(const FILETIME& ft) {
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return {};
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%04u-%02u-%02uT%02u:%02u:%02uZ", st.wYear, st.wMonth, st.wDay,
                  st.wHour, st.wMinute, st.wSecond);
    return buffer;
}

std::string now_iso() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return filetime_to_iso(ft);
}

[[noreturn]] void throw_win_error(const char* operation, DWORD error) {
    if (error == ERROR_NO_SUCH_LOGON_SESSION) {
        throw CredentialError("CREDENTIAL_STORE_UNAVAILABLE",
                              "Windows Credential Manager is not available in this logon session");
    }
    throw CredentialError("CREDENTIAL_STORE_ERROR",
                          std::string(operation) + " failed with Windows error " + std::to_string(error));
}

std::string attribute_value(const CREDENTIALW& credential, std::wstring_view keyword) {
    for (DWORD i = 0; i < credential.AttributeCount; ++i) {
        const auto& attribute = credential.Attributes[i];
        if (attribute.Keyword && keyword == attribute.Keyword && attribute.Value) {
            return std::string(reinterpret_cast<const char*>(attribute.Value), attribute.ValueSize);
        }
    }
    return {};
}

CredentialSummary summary_from(const CREDENTIALW& credential) {
    CredentialSummary summary;
    if (credential.TargetName) {
        summary.connection = connection_from_target(credential.TargetName).value_or(std::string());
    }
    if (credential.UserName) summary.username = narrow(credential.UserName);
    summary.client = attribute_value(credential, L"fairyfly:client");
    summary.language = attribute_value(credential, L"fairyfly:language");
    summary.updated_at = filetime_to_iso(credential.LastWritten);
    return summary;
}

} // namespace

nlohmann::json CredentialSummary::to_json() const {
    return {{"connection", connection}, {"username", username}, {"client", client},
            {"language", language},     {"updated_at", updated_at}};
}

std::string trim_connection_name(std::string_view connection) {
    const auto first = connection.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = connection.find_last_not_of(" \t\r\n");
    return std::string(connection.substr(first, last - first + 1));
}

std::wstring target_name(const std::string& connection) {
    const std::string trimmed = trim_connection_name(connection);
    const bool has_control = std::any_of(trimmed.begin(), trimmed.end(),
                                         [](unsigned char c) { return c < 0x20 || c == 0x7F; });
    if (trimmed.empty() || trimmed.size() > kMaxConnectionName || has_control) {
        throw CredentialError("INVALID_CONNECTION_NAME",
                              "Connection name must be 1-200 characters without control characters");
    }
    return std::wstring(kTargetPrefixW) + widen(trimmed);
}

std::optional<std::string> connection_from_target(std::wstring_view target) {
    if (target.size() <= kTargetPrefixW.size() || target.substr(0, kTargetPrefixW.size()) != kTargetPrefixW) {
        return std::nullopt;
    }
    std::string name = narrow(target.substr(kTargetPrefixW.size()));
    if (name.empty()) return std::nullopt;
    return name;
}

std::vector<unsigned char> encode_password_blob(const std::string& utf8) {
    std::wstring wide = widen(utf8);
    const size_t bytes = wide.size() * sizeof(wchar_t);
    if (bytes > kMaxPasswordBlobBytes) {
        scrub_string(wide);
        throw CredentialError("PASSWORD_TOO_LONG", "Password exceeds the Credential Manager limit of 1280 characters");
    }
    std::vector<unsigned char> blob(bytes);
    if (bytes) std::memcpy(blob.data(), wide.data(), bytes);
    scrub_string(wide);
    return blob;
}

SecretBuffer decode_password_blob(const unsigned char* data, size_t size) {
    std::wstring wide(size / sizeof(wchar_t), L'\0');
    if (!wide.empty()) std::memcpy(wide.data(), data, wide.size() * sizeof(wchar_t));
    return SecretBuffer(std::move(wide));
}

// ---------------------------------------------------------------- Windows store

std::optional<StoredCredential> WindowsCredentialStore::read(const std::string& connection) {
    const std::wstring target = target_name(connection);
    PCREDENTIALW raw = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &raw)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) return std::nullopt;
        throw_win_error("CredReadW", error);
    }
    CredentialPtr credential(raw);
    StoredCredential stored;
    stored.meta = summary_from(*credential);
    stored.meta.connection = trim_connection_name(connection);
    stored.password = decode_password_blob(credential->CredentialBlob, credential->CredentialBlobSize);
    return stored;
}

void WindowsCredentialStore::write(const std::string& connection, const CredentialSummary& meta,
                                   const SecretBuffer& password) {
    const std::wstring target = target_name(connection);
    std::vector<unsigned char> blob = encode_password_blob(password.utf8());
    struct BlobScrubber {
        std::vector<unsigned char>& v;
        ~BlobScrubber() {
            if (!v.empty()) SecureZeroMemory(v.data(), v.size());
        }
    } scrubber{blob};

    if (meta.client.size() > kMaxAttributeBytes || meta.language.size() > kMaxAttributeBytes) {
        throw CredentialError("INVALID_ARGUMENT", "Client or language value is too long");
    }
    std::wstring user = widen(meta.username);
    std::wstring comment = L"fairyfly SAP GUI logon";
    std::wstring key_schema = L"fairyfly:schema";
    std::wstring key_client = L"fairyfly:client";
    std::wstring key_language = L"fairyfly:language";
    std::string schema = "1";
    std::string client = meta.client;
    std::string language = meta.language;

    CREDENTIAL_ATTRIBUTEW attributes[3]{};
    attributes[0].Keyword = key_schema.data();
    attributes[0].ValueSize = static_cast<DWORD>(schema.size());
    attributes[0].Value = reinterpret_cast<LPBYTE>(schema.data());
    DWORD count = 1;
    if (!client.empty()) {
        attributes[count].Keyword = key_client.data();
        attributes[count].ValueSize = static_cast<DWORD>(client.size());
        attributes[count].Value = reinterpret_cast<LPBYTE>(client.data());
        ++count;
    }
    if (!language.empty()) {
        attributes[count].Keyword = key_language.data();
        attributes[count].ValueSize = static_cast<DWORD>(language.size());
        attributes[count].Value = reinterpret_cast<LPBYTE>(language.data());
        ++count;
    }

    std::wstring target_copy = target;
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target_copy.data();
    credential.Comment = comment.data();
    credential.CredentialBlobSize = static_cast<DWORD>(blob.size());
    credential.CredentialBlob = blob.empty() ? nullptr : blob.data();
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.AttributeCount = count;
    credential.Attributes = attributes;
    credential.UserName = user.data();
    if (!CredWriteW(&credential, 0)) throw_win_error("CredWriteW", GetLastError());
}

bool WindowsCredentialStore::remove(const std::string& connection) {
    const std::wstring target = target_name(connection);
    if (CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_NOT_FOUND) return false;
    throw_win_error("CredDeleteW", error);
}

std::vector<CredentialSummary> WindowsCredentialStore::list() {
    PCREDENTIALW* items = nullptr;
    DWORD count = 0;
    if (!CredEnumerateW(L"fairyfly:*", 0, &count, &items)) {
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_FOUND) return {};
        throw_win_error("CredEnumerateW", error);
    }
    CredentialArray array(items, count);
    std::vector<CredentialSummary> result;
    for (size_t i = 0; i < array.size(); ++i) {
        if (array[i].Type != CRED_TYPE_GENERIC) continue;
        CredentialSummary summary = summary_from(array[i]);
        if (summary.connection.empty()) continue;
        result.push_back(std::move(summary));
    }
    std::sort(result.begin(), result.end(),
              [](const CredentialSummary& a, const CredentialSummary& b) { return a.connection < b.connection; });
    return result;
}

// ---------------------------------------------------------------- in-memory store

std::optional<StoredCredential> InMemoryCredentialStore::read(const std::string& connection) {
    const auto it = entries_.find(trim_connection_name(connection));
    if (it == entries_.end()) return std::nullopt;
    StoredCredential stored;
    stored.meta = it->second.meta;
    stored.password = SecretBuffer(std::string(it->second.password.utf8()));
    return stored;
}

void InMemoryCredentialStore::write(const std::string& connection, const CredentialSummary& meta,
                                    const SecretBuffer& password) {
    const std::string name = trim_connection_name(connection);
    (void)target_name(name);  // same validation as the real store
    (void)encode_password_blob(password.utf8());  // same size limit as the real store
    Entry entry;
    entry.meta = meta;
    entry.meta.connection = name;
    entry.meta.updated_at = now_iso();
    entry.password = SecretBuffer(std::string(password.utf8()));
    entries_.erase(name);
    entries_.emplace(name, std::move(entry));
}

bool InMemoryCredentialStore::remove(const std::string& connection) {
    return entries_.erase(trim_connection_name(connection)) > 0;
}

std::vector<CredentialSummary> InMemoryCredentialStore::list() {
    std::vector<CredentialSummary> result;
    for (const auto& [name, entry] : entries_) result.push_back(entry.meta);
    return result;
}

std::unique_ptr<CredentialStore> make_default_store() {
    return std::make_unique<WindowsCredentialStore>();
}

} // namespace fairyfly::cred
