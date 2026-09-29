#include <catch2/catch_test_macros.hpp>

#include "include/credential_resolver.h"

#include <filesystem>
#include <fstream>
#include <sstream>

using namespace fairyfly::cred;
using Status = fairyfly::ResultT<ResolvedLogin>::Status;

namespace {

const char* kFile = "Username: FILEUSER\nPassword: file-pw\nSystem ID: 100\n";
const char* kStdin = "Username: STDINUSER\nPassword: stdin-pw\nSystem ID: 200\n";

OpenFileFn file_returning(const std::string& content, int* calls = nullptr) {
    return [content, calls](const std::string&) -> std::unique_ptr<std::istream> {
        if (calls) ++*calls;
        return std::make_unique<std::istringstream>(content);
    };
}

OpenFileFn no_file_expected(int* calls) {
    return [calls](const std::string&) -> std::unique_ptr<std::istream> {
        ++*calls;
        return nullptr;
    };
}

void seed(InMemoryCredentialStore& store, const std::string& name, const std::string& user) {
    store.write(name, CredentialSummary{"", user, "001", "DE", ""}, SecretBuffer(std::string("store-pw")));
}

} // namespace

TEST_CASE("resolver prefers stdin over file and store", "[cred][resolver]") {
    InMemoryCredentialStore store;
    seed(store, "Bigfox", "STOREUSER");
    std::istringstream in(kStdin);
    LoginSource source;
    source.from_stdin = true;
    source.credentials_file = "ignored.env";
    int calls = 0;
    const auto result = resolve_login_credentials(source, "Bigfox", store, in, file_returning(kFile, &calls));
    REQUIRE(result.status == Status::Success);
    REQUIRE(result.value.source == "stdin");
    REQUIRE(result.value.credentials.username == "STDINUSER");
    REQUIRE(result.value.warnings.empty());
    REQUIRE(calls == 0);
}

TEST_CASE("resolver reads the deprecated file before the store and warns", "[cred][resolver]") {
    InMemoryCredentialStore store;
    seed(store, "Bigfox", "STOREUSER");
    std::istringstream in;
    LoginSource source;
    source.credentials_file = "creds.env";
    const auto result = resolve_login_credentials(source, "Bigfox", store, in, file_returning(kFile));
    REQUIRE(result.status == Status::Success);
    REQUIRE(result.value.source == "file");
    REQUIRE(result.value.credentials.username == "FILEUSER");
    REQUIRE(result.value.warnings.size() == 1);
    REQUIRE(result.value.warnings[0] == "deprecated: use `fairyfly credentials import-env` then `login`");
}

TEST_CASE("resolver uses the store by default connection name and credential_name overrides", "[cred][resolver]") {
    InMemoryCredentialStore store;
    seed(store, "Bigfox", "BIGUSER");
    seed(store, "Other", "OTHERUSER");
    std::istringstream in;
    int calls = 0;

    auto by_default = resolve_login_credentials(LoginSource{}, "Bigfox", store, in, no_file_expected(&calls));
    REQUIRE(by_default.status == Status::Success);
    REQUIRE(by_default.value.source == "credential-manager");
    REQUIRE(by_default.value.credentials.username == "BIGUSER");
    REQUIRE(by_default.value.credentials.password == "store-pw");
    REQUIRE(by_default.value.credentials.client == "001");
    REQUIRE(by_default.value.credentials.language == "DE");

    LoginSource named;
    named.credential_name = "Other";
    auto overridden = resolve_login_credentials(named, "Bigfox", store, in, no_file_expected(&calls));
    REQUIRE(overridden.status == Status::Success);
    REQUIRE(overridden.value.credentials.username == "OTHERUSER");
    REQUIRE(calls == 0);
}

TEST_CASE("resolver reports CREDENTIALS_NOT_FOUND with a suggestion and never reads trial.env", "[cred][resolver]") {
    namespace fs = std::filesystem;
    const fs::path original = fs::current_path();
    const fs::path dir = fs::temp_directory_path() / "fairyfly_resolver_test";
    fs::create_directories(dir);
    {
        std::ofstream decoy(dir / "trial.env");
        decoy << "Username: DECOY\nPassword: decoy\nSystem ID: 001\n";  // dummy, not a real credential
    }
    fs::current_path(dir);
    struct Restore {
        fs::path original, dir;
        ~Restore() {
            std::error_code ec;
            fs::current_path(original, ec);
            fs::remove_all(dir, ec);
        }
    } restore{original, dir};

    InMemoryCredentialStore store;
    std::istringstream in;
    int calls = 0;
    const auto result = resolve_login_credentials(LoginSource{}, "Missing", store, in, no_file_expected(&calls));
    REQUIRE(result.status == Status::Error);
    REQUIRE(result.error.at("code") == "CREDENTIALS_NOT_FOUND");
    REQUIRE(result.error.at("suggestions").at(0) == "fairyfly credentials set Missing");
    REQUIRE(calls == 0);

    const auto nameless = resolve_login_credentials(LoginSource{}, "", store, in, no_file_expected(&calls));
    REQUIRE(nameless.error.at("code") == "CREDENTIALS_NOT_FOUND");
    REQUIRE(calls == 0);
}

TEST_CASE("resolver reports unreadable and malformed files", "[cred][resolver]") {
    InMemoryCredentialStore store;
    std::istringstream in;
    int calls = 0;
    LoginSource source;
    source.credentials_file = "missing.env";
    REQUIRE(resolve_login_credentials(source, "", store, in, no_file_expected(&calls)).error.at("code") ==
            "CREDENTIAL_FILE_UNAVAILABLE");
    REQUIRE(resolve_login_credentials(source, "", store, in, file_returning("nothing useful\n")).error.at("code") ==
            "INVALID_CREDENTIAL_FILE");
}

TEST_CASE("scrub_login_credentials clears the secret strings", "[cred][resolver]") {
    fairyfly::LoginCredentials credentials;
    credentials.username = "U";
    credentials.password = "pw";
    credentials.new_password = "pw2";
    scrub_login_credentials(credentials);
    REQUIRE(credentials.password.empty());
    REQUIRE(credentials.new_password.empty());
    REQUIRE(credentials.username == "U");
}
