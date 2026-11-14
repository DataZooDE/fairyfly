#include "include/credential_resolver.h"

#include <algorithm>
#include <cctype>

namespace fairyfly::cred {

namespace {

using Resolved = ResultT<ResolvedLogin>;

Resolved failure(const std::string& code, const std::string& message, nlohmann::json suggestions = nullptr) {
    Resolved result;
    result.status = Resolved::Status::Error;
    result.error = {{"code", code}, {"message", message}};
    if (!suggestions.is_null()) result.error["suggestions"] = std::move(suggestions);
    return result;
}

Resolved parse_source(std::istream& stream, const char* source) {
    Resolved result;
    try {
        result.value.credentials = parse_login_credentials(stream);
    } catch (const std::invalid_argument& e) {
        return failure("INVALID_CREDENTIAL_FILE", e.what());
    }
    result.status = Resolved::Status::Success;
    result.value.source = source;
    return result;
}

} // namespace

std::string shell_argument(const std::string& value) {
    const bool plain = !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.' || c >= 0x80;
    });
    if (plain) return value;
    if (value.empty() || value.find_first_of("\"`$%") != std::string::npos) return "<name>";
    return "\"" + value + "\"";
}

std::vector<std::string> stored_credential_problems(const CredentialSummary& meta) {
    std::vector<std::string> problems;
    if (!valid_sap_user(meta.username)) problems.push_back("no SAP user");
    if (!valid_sap_client(meta.client)) problems.push_back("no SAP client");
    return problems;
}

void scrub_login_credentials(LoginCredentials& credentials) noexcept {
    scrub_string(credentials.password);
    scrub_string(credentials.new_password);
}

ResultT<ResolvedLogin> resolve_login_credentials(const LoginSource& source,
                                                 const std::string& default_connection,
                                                 CredentialStore& store,
                                                 std::istream& stdin_stream,
                                                 const OpenFileFn& open_file) {
    if (source.from_stdin) return parse_source(stdin_stream, "stdin");

    if (source.credentials_file && !source.credentials_file->empty()) {
        std::unique_ptr<std::istream> file = open_file ? open_file(*source.credentials_file) : nullptr;
        if (!file || !*file) return failure("CREDENTIAL_FILE_UNAVAILABLE", "Cannot open credential file");
        Resolved result = parse_source(*file, "file");
        if (result.status == Resolved::Status::Success) {
            result.value.warnings.push_back(
                "deprecated: use `fairyfly credentials import-env` then `session login`");
        }
        return result;
    }

    const std::string name = trim_connection_name(
        source.credential_name && !source.credential_name->empty() ? *source.credential_name : default_connection);
    if (!name.empty()) {
        try {
            std::optional<StoredCredential> stored = store.read(name);
            if (stored) {
                // Refuse before any logon field is filled: an empty client would blank the logon screen's
                // client and leave the automatic-login guard without an SAP identity.
                if (const auto problems = stored_credential_problems(stored->meta); !problems.empty()) {
                    std::string what = problems.front();
                    for (std::size_t i = 1; i < problems.size(); ++i) what += " and " + problems[i];
                    std::string user = valid_sap_user(stored->meta.username) ? shell_argument(stored->meta.username) : "<USER>";
                    if (user == "<name>") user = "<USER>";
                    return failure("CREDENTIALS_INCOMPLETE",
                                   "Stored credentials for '" + name + "' have " + what +
                                       "; the entry was probably edited outside fairyfly",
                                   nlohmann::json::array(
                                       {"fairyfly credentials set " + shell_argument(name) + " --user " + user + " --client <CLIENT>",
                                        "Edit fairyfly entries only with `fairyfly credentials set`: the Windows "
                                        "Credential Manager window removes the SAP client"}));
                }
                Resolved result;
                result.status = Resolved::Status::Success;
                result.value.source = "credential-manager";
                auto& credentials = result.value.credentials;
                credentials.connection = name;
                credentials.username = stored->meta.username;
                credentials.password = stored->password.utf8();
                credentials.client = stored->meta.client;
                if (!stored->meta.language.empty()) credentials.language = stored->meta.language;
                return result;
            }
        } catch (const CredentialError& e) {
            return failure(e.code(), e.what());
        }
    }

    const std::string shown = shell_argument(name);
    return failure("CREDENTIALS_NOT_FOUND",
                   name.empty() ? "No credentials given and no connection name to look them up by"
                                : "No stored credentials for connection '" + name + "'",
                   nlohmann::json::array({"fairyfly credentials set " + shown}));
}

} // namespace fairyfly::cred
