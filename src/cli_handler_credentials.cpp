// CommandHandler entry points for the `credentials` command (credential store management).
//
// These handlers deliberately do NOT consult read_only_: `credentials set|delete|import-env`
// only change the local credential store, never SAP state, so --read-only allows them.
// No secret is ever put into a Result, a log line or an exception message.
#include "include/cli_handler.h"
#include "include/console_prompt.h"
#include "include/login_flow.h"
#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>

namespace fairyfly {
namespace cli {

namespace {

Result error_result(const std::string& code, const std::string& message) {
    Result result;
    result.status = Result::Status::Error;
    result.error = {{"code", code}, {"message", message}};
    return result;
}

Result store_error(const cred::CredentialError& e) { return error_result(e.code(), e.what()); }

bool valid_client(const std::string& client) {
    return client.size() == 3 &&
           std::all_of(client.begin(), client.end(), [](unsigned char c) { return std::isdigit(c); });
}

Result prompt_unavailable(const char* command) {
    Result result = error_result("CREDENTIALS_PROMPT_UNAVAILABLE",
                                 std::string("credentials ") + command +
                                     " needs an interactive prompt or standard input, which batch mode does not provide");
    result.error["suggestions"] = nlohmann::json::array(
        {"Run `fairyfly credentials " + std::string(command) + "` outside of batch"});
    return result;
}

} // namespace

Result CommandHandler::handle_credentials_set(const std::string& connection, const std::string& user,
                                              const std::string& client, const std::string& language,
                                              bool password_stdin)
{
    if (batch_mode_) return prompt_unavailable("set");
    if (cred::trim_connection_name(connection).empty()) {
        return error_result("INVALID_ARGUMENT", "A connection name is required");
    }
    if (user.empty()) return error_result("INVALID_ARGUMENT", "--user is required");
    if (!valid_client(client)) return error_result("INVALID_ARGUMENT", "--client must be a three-digit SAP client");

    try {
        cred::SecretBuffer password;
        if (password_stdin || !cred::stdin_is_console()) {
            password = cred::read_secret("", cred::PromptSource::Stdin);
        } else {
            password = cred::read_secret("SAP password: ", cred::PromptSource::Console);
            cred::SecretBuffer confirm = cred::read_secret("Repeat password: ", cred::PromptSource::Console);
            if (password.utf16() != confirm.utf16()) {
                return error_result("PASSWORD_MISMATCH", "The two passwords are not identical; nothing was stored");
            }
        }
        if (password.empty()) return error_result("EMPTY_PASSWORD", "No password was provided; nothing was stored");

        cred::CredentialSummary meta;
        meta.username = user;
        meta.client = client;
        meta.language = language.empty() ? "EN" : language;
        credential_store().write(connection, meta, password);

        auto stored = credential_store().read(connection);
        Result result;
        result.status = Result::Status::Success;
        result.data["credential"] = stored ? stored->meta.to_json() : nlohmann::json(meta.to_json());
        result.data["message"] = "Credential stored for connection '" + cred::trim_connection_name(connection) + "'";
        return result;
    } catch (const cred::CredentialError& e) {
        return store_error(e);
    }
}

Result CommandHandler::handle_credentials_list()
{
    try {
        Result result;
        result.status = Result::Status::Success;
        result.data["credentials"] = nlohmann::json::array();
        const auto entries = credential_store().list();
        for (const auto& entry : entries) result.data["credentials"].push_back(entry.to_json());
        result.data["count"] = entries.size();
        return result;
    } catch (const cred::CredentialError& e) {
        return store_error(e);
    }
}

Result CommandHandler::handle_credentials_delete(const std::string& connection)
{
    try {
        const bool removed = credential_store().remove(connection);
        if (!removed) {
            return error_result("CREDENTIALS_NOT_FOUND",
                                "No stored credentials for connection '" + cred::trim_connection_name(connection) + "'");
        }
        Result result;
        result.status = Result::Status::Success;
        result.data["message"] = "Credential deleted for connection '" + cred::trim_connection_name(connection) + "'";
        return result;
    } catch (const cred::CredentialError& e) {
        return store_error(e);
    }
}

Result CommandHandler::handle_credentials_import_env(const std::string& path, const std::string& connection,
                                                     bool delete_file)
{
    if (batch_mode_) return prompt_unavailable("import-env");

    std::ifstream file(path, std::ios::binary);
    if (!file) return error_result("CREDENTIAL_FILE_UNAVAILABLE", "Cannot open credential file");

    LoginCredentials credentials;
    try {
        credentials = parse_login_credentials(file);
    } catch (const std::invalid_argument& e) {
        return error_result("INVALID_CREDENTIAL_FILE", e.what());
    }
    file.close();

    // Scrub every parsed secret on every exit path.
    struct Scrubber {
        LoginCredentials& c;
        ~Scrubber() {
            cred::scrub_string(c.password);
            cred::scrub_string(c.new_password);
        }
    } scrubber{credentials};

    std::string name = !credentials.connection.empty() ? credentials.connection : connection;
    name = cred::trim_connection_name(name);
    if (name.empty()) {
        return error_result("CONNECTION_NAME_REQUIRED",
                            "The file has no `Connection:` line; pass --connection NAME");
    }

    try {
        cred::CredentialSummary meta;
        meta.username = credentials.username;
        meta.client = credentials.client;
        meta.language = credentials.language;
        // new_password (forced change) is never persisted.
        credential_store().write(name, meta, cred::SecretBuffer(std::string(credentials.password)));
    } catch (const cred::CredentialError& e) {
        return store_error(e);
    }

    Result result;
    result.status = Result::Status::Success;
    auto stored = credential_store().read(name);
    result.data["credential"] = stored ? stored->meta.to_json() : nlohmann::json::object();
    result.data["message"] = "Imported credential for connection '" + name + "'";
    nlohmann::json warnings = nlohmann::json::array();
    warnings.push_back("The password was stored in the Windows Credential Manager; rotate it if the source file was ever shared or committed");
    if (delete_file) {
        if (std::remove(path.c_str()) == 0) {
            result.data["file_deleted"] = true;
        } else {
            result.data["file_deleted"] = false;
            warnings.push_back("Could not delete " + path + "; remove it manually");
        }
    } else {
        warnings.push_back("The source file still contains the password; delete it or re-run with --delete-file");
    }
    result.data["warnings"] = warnings;
    return result;
}

} // namespace cli
} // namespace fairyfly
