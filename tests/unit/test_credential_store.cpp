#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "include/cli_handler.h"
#include "include/commands/command_base.h"
#include "include/console_prompt.h"
#include "include/credential_store.h"
#include "include/login_flow.h"

#include <CLI/CLI.hpp>

#include <new>
#include <sstream>

namespace fairyfly::commands {
std::unique_ptr<CommandBase> create_credentials_command();
}

using namespace fairyfly::cred;

namespace {
bool all_zero(const char* data, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (data[i] != 0) return false;
    }
    return true;
}
} // namespace

TEST_CASE("target_name trims and prefixes the connection name", "[cred]") {
    REQUIRE(target_name("Bigfox") == L"fairyfly:Bigfox");
    REQUIRE(target_name("  Bigfox \t") == L"fairyfly:Bigfox");
    REQUIRE_THROWS_AS(target_name("   "), CredentialError);
    REQUIRE_THROWS_AS(target_name(""), CredentialError);
}

TEST_CASE("connection_from_target inverts target_name and rejects foreign targets", "[cred]") {
    REQUIRE(connection_from_target(target_name("Bigfox")) == std::optional<std::string>("Bigfox"));
    REQUIRE(connection_from_target(target_name("Prod System")) == std::optional<std::string>("Prod System"));
    REQUIRE_FALSE(connection_from_target(L"git:https://github.com").has_value());
    REQUIRE_FALSE(connection_from_target(L"fairyfly:").has_value());
    REQUIRE_FALSE(connection_from_target(L"").has_value());
}

TEST_CASE("password blob round-trips UTF-8 without a trailing NUL", "[cred]") {
    const std::string password = "p\xC3\xA4ss:w\xC3\xB6rd \xE2\x82\xAC";  // non-ASCII: a-umlaut, o-umlaut, euro
    const auto blob = encode_password_blob(password);
    const std::wstring expected = L"päss:wörd €";
    REQUIRE(blob.size() == expected.size() * sizeof(wchar_t));
    // No terminating NUL character.
    REQUIRE_FALSE((blob[blob.size() - 1] == 0 && blob[blob.size() - 2] == 0));
    const SecretBuffer decoded = decode_password_blob(blob.data(), blob.size());
    REQUIRE(decoded.utf8() == password);
    REQUIRE(decoded.utf16() == expected);
}

TEST_CASE("password blobs above 2560 bytes are rejected", "[cred]") {
    REQUIRE_NOTHROW(encode_password_blob(std::string(1280, 'a')));  // 2560 bytes exactly
    try {
        encode_password_blob(std::string(1281, 'a'));
        FAIL("expected PASSWORD_TOO_LONG");
    } catch (const CredentialError& e) {
        REQUIRE(e.code() == "PASSWORD_TOO_LONG");
    }
}

TEST_CASE("CredentialSummary JSON contains no password key", "[cred]") {
    CredentialSummary summary{"Bigfox", "DEVELOPER", "001", "EN", "2026-01-02T03:04:05Z"};
    const auto json = summary.to_json();
    REQUIRE(json.at("connection") == "Bigfox");
    REQUIRE(json.at("username") == "DEVELOPER");
    REQUIRE(json.at("client") == "001");
    REQUIRE(json.at("language") == "EN");
    REQUIRE(json.at("updated_at") == "2026-01-02T03:04:05Z");
    for (const auto& item : json.items()) {
        REQUIRE(item.key().find("pass") == std::string::npos);
        REQUIRE(item.key().find("secret") == std::string::npos);
    }
}

TEST_CASE("scrub_string zeroes the characters", "[cred]") {
    std::string value = "secret";  // short: stored inline, so the bytes stay inspectable
    const char* bytes = value.data();
    scrub_string(value);
    REQUIRE(value.empty());
    REQUIRE(all_zero(bytes, 6));

    std::wstring wide = L"secret";
    const wchar_t* wbytes = wide.data();
    scrub_string(wide);
    REQUIRE(wide.empty());
    REQUIRE(all_zero(reinterpret_cast<const char*>(wbytes), 6 * sizeof(wchar_t)));
}

TEST_CASE("SecretBuffer zeroes its storage on destruction and after a move", "[cred]") {
    alignas(SecretBuffer) unsigned char storage[sizeof(SecretBuffer)];
    auto* first = new (storage) SecretBuffer(std::string("hunter2"));
    const char* bytes = first->utf8().data();  // inline (short string) bytes inside `storage`
    REQUIRE(std::string(bytes) == "hunter2");

    SecretBuffer moved(std::move(*first));
    REQUIRE(first->empty());
    REQUIRE(all_zero(bytes, 7));  // moved-from buffer was scrubbed
    REQUIRE(moved.utf8() == "hunter2");

    first->~SecretBuffer();
    REQUIRE(all_zero(reinterpret_cast<const char*>(storage), 7));

    alignas(SecretBuffer) unsigned char storage2[sizeof(SecretBuffer)];
    auto* second = new (storage2) SecretBuffer(std::string("abc123"));
    const char* bytes2 = second->utf8().data();
    second->~SecretBuffer();
    REQUIRE(all_zero(bytes2, 6));
}

TEST_CASE("InMemoryCredentialStore write, read, list and remove", "[cred]") {
    InMemoryCredentialStore store;
    REQUIRE_FALSE(store.read("Bigfox").has_value());
    REQUIRE(store.list().empty());

    store.write(" Bigfox ", CredentialSummary{"", "DEVELOPER", "001", "EN", ""}, SecretBuffer(std::string("pw-1")));
    store.write("Alpha", CredentialSummary{"", "ZFFTEST", "100", "DE", ""}, SecretBuffer(std::string("pw-2")));

    auto stored = store.read("Bigfox");
    REQUIRE(stored.has_value());
    REQUIRE(stored->meta.connection == "Bigfox");
    REQUIRE(stored->meta.username == "DEVELOPER");
    REQUIRE(stored->password.utf8() == "pw-1");

    store.write("Bigfox", CredentialSummary{"", "OTHER", "002", "EN", ""}, SecretBuffer(std::string("pw-3")));
    REQUIRE(store.read("Bigfox")->meta.username == "OTHER");
    REQUIRE(store.read("Bigfox")->password.utf8() == "pw-3");

    const auto listed = store.list();
    REQUIRE(listed.size() == 2);
    REQUIRE_FALSE(store.remove("Missing"));
    REQUIRE(store.remove("Alpha"));
    REQUIRE(store.list().size() == 1);
}

TEST_CASE("stdin secret line strips BOM and CRLF", "[cred]") {
    std::istringstream input("\xEF\xBB\xBFtopsecret\r\nsecond line\r\n");
    const SecretBuffer secret = read_secret_line(input);
    REQUIRE(secret.utf8() == "topsecret");
    std::istringstream plain("abc\n");
    REQUIRE(read_secret_line(plain).utf8() == "abc");
    std::istringstream empty("");
    REQUIRE(read_secret_line(empty).empty());
}

TEST_CASE("parse_login_credentials reads Connection and still ignores unknown keys", "[cred][login]") {
    std::istringstream input(
        "connection: Bigfox\nUsername: DEVELOPER\nPassword: a:b\nSystem ID: 001\nLanguage: DE\nColor: blue\n");
    const auto credentials = fairyfly::parse_login_credentials(input);
    REQUIRE(credentials.connection == "Bigfox");
    REQUIRE(credentials.username == "DEVELOPER");
    REQUIRE(credentials.client == "001");
    REQUIRE(credentials.language == "DE");

    std::istringstream without("Username: U\nPassword: p\nSystem ID: 001\n");
    REQUIRE(fairyfly::parse_login_credentials(without).connection.empty());
}

TEST_CASE("credentials set is refused in batch mode", "[cred][cli]") {
    fairyfly::cli::CommandHandler handler(std::make_unique<InMemoryCredentialStore>());
    handler.set_batch_mode(true);
    const auto result = handler.handle_credentials_set("Bigfox", "DEVELOPER", "001", "EN", true);
    REQUIRE(result.status == fairyfly::Result::Status::Error);
    REQUIRE(result.error.at("code") == "CREDENTIALS_PROMPT_UNAVAILABLE");
    const auto import = handler.handle_credentials_import_env("trial.env", "Bigfox", false);
    REQUIRE(import.error.at("code") == "CREDENTIALS_PROMPT_UNAVAILABLE");
    REQUIRE(handler.credential_store().list().empty());
}

TEST_CASE("credentials list and delete work against an injected store", "[cred][cli]") {
    fairyfly::cli::CommandHandler handler(std::make_unique<InMemoryCredentialStore>());
    handler.credential_store().write("Bigfox", CredentialSummary{"", "DEVELOPER", "001", "EN", ""},
                                     SecretBuffer(std::string("pw")));
    const auto listed = handler.handle_credentials_list();
    REQUIRE(listed.status == fairyfly::Result::Status::Success);
    REQUIRE(listed.data.at("count") == 1);
    REQUIRE(listed.data.dump().find("pw") == std::string::npos);
    REQUIRE(handler.handle_credentials_delete("Bigfox").status == fairyfly::Result::Status::Success);
    REQUIRE(handler.handle_credentials_delete("Bigfox").error.at("code") == "CREDENTIALS_NOT_FOUND");
}

TEST_CASE("the credentials command has no password option", "[cred][cli]") {
    CLI::App app;
    auto command = fairyfly::commands::create_credentials_command();
    CLI::App* credentials = command->setup_cli(app);
    REQUIRE(credentials != nullptr);
    std::vector<CLI::App*> commands{credentials};
    for (auto* sub : credentials->get_subcommands(nullptr)) commands.push_back(sub);
    REQUIRE(commands.size() == 5);
    for (auto* cmd : commands) {
        for (auto* option : cmd->get_options()) {
            REQUIRE_FALSE(option->check_lname("password"));
            REQUIRE_FALSE(option->check_sname("p"));
        }
    }
    // The only password-related switch is the stdin flag on `set`.
    REQUIRE(credentials->get_subcommand("set")->get_option_no_throw("--password-stdin") != nullptr);
    REQUIRE(credentials->get_subcommand("set")->get_option_no_throw("--password") == nullptr);
}

TEST_CASE("Windows Credential Manager round trip", "[!mayfail][credstore]") {
    WindowsCredentialStore store;
    const std::string name = "__fairyfly_unit_test__";
    const std::string password = "unit-test-\xC3\xA4-pw";
    try {
        store.write(name, CredentialSummary{"", "TESTUSER", "001", "EN", ""}, SecretBuffer(std::string(password)));
    } catch (const CredentialError& e) {
        if (e.code() == "CREDENTIAL_STORE_UNAVAILABLE") SKIP("No credential store in this logon session");
        throw;
    }
    struct Cleanup {
        WindowsCredentialStore& s;
        std::string n;
        ~Cleanup() { try { s.remove(n); } catch (...) {} }
    } cleanup{store, name};

    const auto stored = store.read(name);
    REQUIRE(stored.has_value());
    REQUIRE(stored->meta.username == "TESTUSER");
    REQUIRE(stored->meta.client == "001");
    REQUIRE(stored->meta.language == "EN");
    REQUIRE_FALSE(stored->meta.updated_at.empty());
    REQUIRE(stored->password.utf8() == password);

    bool found = false;
    for (const auto& entry : store.list()) found = found || entry.connection == name;
    REQUIRE(found);

    REQUIRE(store.remove(name));
    REQUIRE_FALSE(store.read(name).has_value());
    REQUIRE_FALSE(store.remove(name));
}
