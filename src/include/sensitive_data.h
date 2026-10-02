#pragma once

#include <algorithm>
#include <cctype>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fairyfly::sap {

inline std::string normalize_sensitive_name(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    value.erase(std::remove_if(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) == 0;
    }), value.end());
    return value;
}

inline bool contains_sensitive_utf8_name(const std::string& value) {
    if (std::none_of(value.begin(), value.end(), [](unsigned char c) { return c >= 0x80; }))
        return false;
    std::string folded = value;
    for (char& c : folded) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    }
    for (const auto* marker : {
             "\xE5\xAF\x86\xE7\xA0\x81", // Chinese: 密码
             "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89", // Japanese: パスワード
             "\xD0\x9F\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C", // Russian: Пароль
             "\xD0\xBF\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C", // Russian: пароль
             "\xC5\x9E" "ifre", "\xC5\x9F" "ifre", // Turkish: Şifre, şifre
             "has\xC5\x82" "o", "has\xC5\x81" "o", // Polish: Hasło, HasŁo
             "\xEB\xB9\x84\xEB\xB0\x80\xEB\xB2\x88\xED\x98\xB8", // Korean: 비밀번호
             "\xEC\x95\x94\xED\x98\xB8", // Korean: 암호
             "l\xC3\xB6senord", "L\xC3\xB6senord", "L\xC3\x96SENORD", // Swedish: Lösenord
             "jelsz\xC3\xB3", "Jelsz\xC3\xB3", "JELSZ\xC3\x93", // Hungarian: Jelszó
             "parol\xC4\x83", "Parol\xC4\x83", "PAROL\xC4\x82", // Romanian: Parolă
             "\xCE\xBA\xCF\x89\xCE\xB4\xCE\xB9\xCE\xBA\xCF\x8C\xCF\x82", // Greek: κωδικός
             "\xCE\x9A\xCF\x89\xCE\xB4\xCE\xB9\xCE\xBA\xCF\x8C\xCF\x82", // Greek: Κωδικός
             "\xCE\x9A\xCE\xA9\xCE\x94\xCE\x99\xCE\x9A\xCE\x9F\xCE\xA3", // Greek: ΚΩΔΙΚΟΣ
             "\xD9\x83\xD9\x84\xD9\x85\xD8\xA9 \xD8\xA7\xD9\x84\xD9\x85\xD8\xB1\xD9\x88\xD8\xB1", // Arabic: كلمة المرور
             "\xD9\x83\xD9\x84\xD9\x85\xD8\xA9 \xD8\xA7\xD9\x84\xD8\xB3\xD8\xB1", // Arabic: كلمة السر
             "\xD7\xA1\xD7\x99\xD7\xA1\xD7\x9E\xD7\x94", // Hebrew: סיסמה
             "\xD7\xA1\xD7\x99\xD7\xA1\xD7\x9E\xD7\x90"  // Hebrew: סיסמא
         }) {
        if (folded.find(marker) != std::string::npos) return true;
    }
    return false;
}

inline bool is_sensitive_data_name(const std::string& name) {
    const std::string key = normalize_sensitive_name(name);
    return contains_sensitive_utf8_name(name) ||
           key == "password" || key == "passwd" || key == "pwd" ||
           key == "passwort" || key == "kennwort" ||
           key == "contrasena" || key == "contrasea" || key == "motdepasse" ||
           key == "senha" || key == "wachtwoord" ||
           key == "passord" || key == "adgangskode" || key == "heslo" ||
           key == "salasana" || key == "paroladordine" ||
           key == "secret" || key == "clientsecret" || key == "apikey" ||
           key == "token" || key == "accesstoken" || key == "refreshtoken" ||
           key == "sessiontoken" || key == "xsessiontoken" ||
           key == "authorization" || key == "proxyauthorization" ||
           key == "xcsrftoken" || key == "cookie" || key == "setcookie" ||
           key == "authenticationinfo";
}

inline bool contains_sensitive_data_name(const std::string& value) {
    const auto name = normalize_sensitive_name(value);
    if (is_sensitive_data_name(name)) return true;
    for (const auto* marker : {"password", "passwd", "passwort", "kennwort",
                               "contrasena", "contrasea", "motdepasse", "senha", "wachtwoord",
                               "passord", "adgangskode", "heslo", "salasana", "paroladordine",
                               "secret", "geheim", "apikey", "token", "cookie",
                               "authorization", "authenticationinfo"}) {
        if (name.find(marker) != std::string::npos) return true;
    }
    return contains_sensitive_utf8_name(value) ||
           (name.size() >= 3 && name.compare(name.size() - 3, 3, "pwd") == 0);
}

// ---------------------------------------------------------------------------------------------
// Redaction markers with a reason, and the narrowed secret-name classification used for screen
// reads. `contains_sensitive_data_name` above stays the broad fail-closed test (ABAP source,
// editor and API responses). Screen reads additionally need to show Basis data that merely has a
// credential-sounding name: a state flag such as PASSWORD_EXT_PWD_STATE, a role or a profile.
// ---------------------------------------------------------------------------------------------

namespace redaction_reason {
inline constexpr const char* password_field = "password input field";
inline constexpr const char* password_name = "field name matches password pattern";
inline constexpr const char* secret_name = "field name matches secret pattern";
inline constexpr const char* paired_name = "paired name field is a credential";
inline constexpr const char* label_row = "row label names a credential";
inline constexpr const char* unverified = "could not verify that the field holds no credential";
}  // namespace redaction_reason

// "[REDACTED: <reason>]". The reason is a fixed phrase: it never names the matched pattern list
// and never contains any part of the hidden value.
inline std::string redaction_marker(const char* reason) {
    return std::string("[REDACTED: ") + reason + "]";
}

// True for the bare "[REDACTED]" and for every "[REDACTED: ...]" marker.
inline bool is_redaction_marker(const std::string& value) {
    return value.rfind("[REDACTED", 0) == 0;
}

// Fields that hold a credential or its hash, compared after normalisation.
inline bool is_known_secret_field_name(const std::string& name) {
    if (is_sensitive_data_name(name)) return true;
    const std::string key = normalize_sensitive_name(name);
    for (const auto* known : {"bapipwd", "newpassword", "oldpassword", "confirmpassword",
                              "repeatpassword", "codvn", "bcode", "passcode", "pwdsaltedhash",
                              "passwordhash", "newpwd", "oldpwd"}) {
        if (key == known) return true;
    }
    return false;
}

// Whether the "state flag" exemption below may apply. It is granted ONLY to display data that cannot be typed into
// (labels, grid/report cells and column titles, a field known to be NOT changeable). Everything else, a changeable or
// unknown-changeability input field, a fill target, an id check, keeps the default Deny (fail closed): a changeable field
// named PASSWORD_STATE could hold a real value.
enum class StateExemption { Deny, Allow };

// The vocabulary of state labels shown by display-only credential-state fields (SU01 "Production Password", "locked", ...).
// ONE place, lower-case, letters/digits only (umlauts are folded to ae/oe/ue/ss before the lookup). A value is a state
// label only when EVERY word of it is listed here (or the whole value is a number/date/time, or empty): an opaque short
// token such as "Tr0ub4d" is not, even though it is short, so it stays redacted. Extend this table to recognise more states.
inline const std::vector<std::string>& state_label_vocabulary() {
    static const std::vector<std::string> words = {
        "initial", "productive", "production", "produktiv", "set", "reset", "not", "no", "yes", "ja", "nein", "locked", "unlocked",
        "lock", "gesperrt", "entsperrt", "active", "inactive", "aktiv", "inaktiv", "disabled", "enabled", "deactivated",
        "activated", "valid", "invalid", "gueltig", "expired", "abgelaufen", "none", "keine", "empty", "blank", "leer",
        "password", "passwort", "pwd", "status", "state", "changed", "geaendert", "required", "erforderlich", "user",
        "system", "technical", "dialog", "service", "reference", "communication", "default", "standard", "ok", "error",
        "warning", "internal", "external", "local", "central", "true", "false", "on", "off", "x", "single", "multiple",
        "ldap", "saml", "snc", "x509", "kerberos", "mixed", "preferred", "only", "allowed", "forbidden", "optional",
        "mandatory", "up", "to", "date", "last", "first", "logon", "login", "never", "since"};
    return words;
}

// A value that plausibly is a state label, not a secret (ALLOWLIST, fail closed): after trimming the padding SAP adds
// to the field width, it is (a) empty, (b) at most 24 characters of digits and date/time separators ("0", "01.10.2026",
// "12:30:00"), or (c) at most 60 characters whose words (split on whitespace, '_' and '-', case-insensitive) are all in
// state_label_vocabulary(). Everything else, including short opaque tokens, is not a state label.
inline bool looks_like_state_value(const std::string& raw) {
    // SAP pads the displayed text of a field to the field width: judge the text without the padding.
    const auto first = raw.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return true;
    const std::string value = raw.substr(first, raw.find_last_not_of(" \t\r\n") - first + 1);
    if (value.size() > 60) return false;
    if (value.size() <= 24 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return std::isdigit(c) || c == '.' || c == '-' || c == '/' || c == ':' || c == ' ';
        }))
        return true;
    // Fold German umlauts (UTF-8) so "gültig" / "geändert" match their vocabulary spelling.
    std::string folded;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c == 0xC3 && i + 1 < value.size()) {
            const unsigned char d = static_cast<unsigned char>(value[i + 1]);
            const char* repl = (d == 0xA4 || d == 0x84) ? "ae" : (d == 0xB6 || d == 0x96) ? "oe"
                             : (d == 0xBC || d == 0x9C) ? "ue" : d == 0x9F ? "ss" : nullptr;
            if (repl) { folded += repl; ++i; continue; }
        }
        folded += static_cast<char>(c);
    }
    const auto& vocabulary = state_label_vocabulary();
    std::string word;
    bool any = false;
    const auto flush = [&]() {
        if (word.empty()) return true;
        any = true;
        const bool known = std::find(vocabulary.begin(), vocabulary.end(), word) != vocabulary.end();
        word.clear();
        return known;
    };
    for (const unsigned char c : folded) {
        if (std::isspace(c) || c == '_' || c == '-') {
            if (!flush()) return false;
        } else if (std::isalnum(c)) {
            word += static_cast<char>(std::tolower(c));
        } else {
            return false;  // any other symbol: not a plain state label
        }
    }
    if (!flush()) return false;
    return any;
}

// A name that only reports the state of a credential (PASSWORD_EXT_PWD_STATE, PASSWORD_STATUS,
// "Password last changed") carries no secret.
inline bool is_credential_state_name(const std::string& name) {
    const std::string key = normalize_sensitive_name(name);
    for (const std::string suffix : {"state", "status", "flag", "indicator", "date", "time", "type",
                                     "policy", "length", "lock", "locked", "attempts", "count",
                                     "valid", "validity", "rule", "rules", "changed", "expired",
                                     "expiry"}) {
        if (key.size() > suffix.size() &&
            key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

inline bool contains_password_word(const std::string& name) {
    const std::string key = normalize_sensitive_name(name);
    for (const auto* word : {"password", "passwd", "passwort", "kennwort", "contrasena", "contrasea",
                             "motdepasse", "senha", "wachtwoord", "passord", "adgangskode", "heslo",
                             "salasana", "paroladordine"}) {
        if (key.find(word) != std::string::npos) return true;
    }
    return contains_sensitive_utf8_name(name) ||
           (key.size() >= 3 && key.compare(key.size() - 3, 3, "pwd") == 0);
}

// Why `name` identifies a credential-bearing field; empty when it does not. State flags are
// exempt unless the name is one of the known credential fields themselves.
inline std::string sensitive_name_reason(const std::string& name, StateExemption state = StateExemption::Deny) {
    const bool known = is_known_secret_field_name(name);
    if (!known && !contains_sensitive_data_name(name)) return {};
    if (!known && state == StateExemption::Allow && is_credential_state_name(name)) return {};
    return contains_password_word(name) ? redaction_reason::password_name
                                        : redaction_reason::secret_name;
}

// SAP technical object names (roles, profiles, programs): no blanks, underscores or namespaces.
inline bool looks_like_technical_name(const std::string& value) {
    if (value.empty() || value.find('_') == std::string::npos) return false;
    return std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '_' || c == '/' || c == '.' || c == '$' || c == '*';
    });
}

// Whether a grid or report cell names a credential, so its row has to be hidden. `scan_substrings`
// additionally treats every caption that merely contains a credential word as a name: for grids
// without business column titles (gateway NAME/VALUE lists) and for positioned report labels.
// Cells of a grid with business column titles (ROLE, PROFILE, ...) only match when they are the
// name itself ("Password"), a "Name: value" / "Name=value" pair or a header name such as
// "X-Session-Token", so role and profile names are not hidden for their words.
inline std::string sensitive_cell_reason(const std::string& cell, bool scan_substrings,
                                         bool technical_names_too = false) {
    if (cell.empty()) return {};
    if (is_known_secret_field_name(cell)) return sensitive_name_reason(cell, StateExemption::Allow);
    const auto pair_at = cell.find_first_of(":=");
    if (pair_at != std::string::npos) {
        const auto left = sensitive_name_reason(cell.substr(0, pair_at), StateExemption::Allow);
        if (!left.empty()) return left;
    }
    const bool header_shaped = cell.find('-') != std::string::npos &&
                               cell.find_first_of(" \t") == std::string::npos;
    if (header_shaped) {
        const auto reason = sensitive_name_reason(cell, StateExemption::Allow);
        if (!reason.empty()) return reason;
    }
    if (scan_substrings && (technical_names_too || !looks_like_technical_name(cell)))
        return sensitive_name_reason(cell, StateExemption::Allow);
    return {};
}

inline std::string sensitive_input_field_reason(const std::string& type, const std::string& id,
                                                const std::string& label,
                                                StateExemption state = StateExemption::Deny) {
    if (type == "GuiPasswordField") return redaction_reason::password_field;
    if (type != "GuiTextField" && type != "GuiCTextField" &&
        type != "GuiComboBox" && type != "GuiComboBoxControl") return {};

    const auto last_separator = id.find_last_of('/');
    const std::string field_name = last_separator == std::string::npos
        ? id : id.substr(last_separator + 1);
    if (auto reason = sensitive_name_reason(field_name, state); !reason.empty()) return reason;
    // USR02-CODVN style names: the part after the table prefix is the SAP field name.
    const auto hyphen = field_name.find_last_of('-');
    if (hyphen != std::string::npos && is_known_secret_field_name(field_name.substr(hyphen + 1)))
        return sensitive_name_reason(field_name.substr(hyphen + 1), state);
    // SAP authorization objects and groups are access-control metadata, not
    // credential values. Keep technical field names authoritative.
    const auto normalized_label = normalize_sensitive_name(label);
    if (normalized_label == "authorizationobject" ||
        normalized_label == "authorizationgroup") return {};
    return sensitive_name_reason(label, state);
}

inline bool is_sensitive_input_field(const std::string& type, const std::string& id,
                                     const std::string& label,
                                     StateExemption state = StateExemption::Deny) {
    return !sensitive_input_field_reason(type, id, label, state).empty();
}

// Reason for a field whose changeability and current value are known to the caller (screen reads, get_text). The state-flag
// exemption applies only when the field is KNOWN to be display-only (changeable_known && !changeable), is not a
// GuiPasswordField and its value looks like a state label; unknown changeability redacts (fail closed).
// `value` may be empty when the caller has not read it yet: it is then asked for through `read_value` (only when needed).
inline std::string sensitive_field_reason_for_display(const std::string& type, const std::string& id,
                                                      const std::string& label, bool changeable_known, bool changeable,
                                                      const std::function<std::string()>& read_value) {
    auto reason = sensitive_input_field_reason(type, id, label, StateExemption::Deny);
    if (reason.empty() || type == "GuiPasswordField") return reason;
    if (!changeable_known || changeable) return reason;
    if (!sensitive_input_field_reason(type, id, label, StateExemption::Allow).empty()) return reason;  // not just a state name
    std::string value;
    try { value = read_value ? read_value() : std::string(); } catch (...) { return reason; }
    if (!looks_like_state_value(value)) return reason;
    return {};
}

// Mask recognizable credential data in text editors and API responses.
std::string redact_sensitive_response_text(const std::string& text);

// Mask ABAP statements that name credential data while preserving source line
// positions for diagnostics, including values continued onto later lines.
std::string redact_sensitive_abap_source(const std::string& text);

// Report screens expose label/value pairs as positioned GuiLabels. Mask the
// whole row when a label identifies credential data, before JSON grouping.
void redact_sensitive_report_labels(nlohmann::json& elements);

// SAP Gateway Client displays HTTP response headers as NAME/VALUE grid rows.
// Keep the header name for diagnostics, but never export credential values.
// Every redacted cell says why: "[REDACTED: <reason>]".
inline bool is_generic_column_title(const std::string& title) {
    if (title.empty()) return true;
    const size_t prefix = title.rfind("Column", 0) == 0 ? 6 : title.rfind("Col", 0) == 0 ? 3 : 0;
    return prefix > 0 && title.size() > prefix &&
           std::all_of(title.begin() + static_cast<std::ptrdiff_t>(prefix), title.end(),
                       [](unsigned char c) { return std::isdigit(c) != 0; });
}

inline void redact_sensitive_header_rows(nlohmann::json& table) {
    if (!table.is_object()) return;
    auto rows_it = table.find("rows");
    if (rows_it == table.end() || !rows_it->is_array()) return;

    int name_column = -1;
    bool any_title = false;
    std::vector<std::pair<size_t, std::string>> sensitive_columns;
    auto columns_it = table.find("columns");
    if (columns_it != table.end() && columns_it->is_array()) {
        const auto& columns = *columns_it;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (!columns[i].is_string()) continue;
            const auto title = columns[i].get<std::string>();
            const auto name = normalize_sensitive_name(title);
            if (name == "name" || name == "headername") name_column = static_cast<int>(i);
            if (!is_generic_column_title(title)) any_title = true;
            auto reason = sensitive_name_reason(title, StateExemption::Allow);
            if (!reason.empty()) sensitive_columns.emplace_back(i, std::move(reason));
        }
    }
    // A grid with business column titles (ROLE, PROFILE, ...) is not a NAME/VALUE list: its cell
    // values are only hidden when they are a credential name themselves.
    const bool business_grid = any_title && name_column < 0;

    for (auto& row : *rows_it) {
        if (!row.is_array()) continue;
        for (const auto& [index, reason] : sensitive_columns) {
            if (index < row.size() && !(row[index].is_string() && row[index].get<std::string>().empty()))
                row[index] = redaction_marker(reason.c_str());
        }

        size_t label_index = row.size();
        std::string label_reason;
        if (name_column >= 0 && static_cast<size_t>(name_column) < row.size() &&
            row[name_column].is_string()) {
            label_reason = sensitive_cell_reason(row[name_column].get<std::string>(), true, true);
            if (!label_reason.empty()) label_index = static_cast<size_t>(name_column);
        }
        if (label_index == row.size()) {
            // Some SAP grids expose no column titles, or localize the value title.
            for (size_t i = 0; i < row.size(); ++i) {
                if (!row[i].is_string() || is_redaction_marker(row[i].get<std::string>())) continue;
                label_reason = sensitive_cell_reason(row[i].get<std::string>(), !business_grid, true);
                if (!label_reason.empty()) {
                    label_index = i;
                    break;
                }
            }
        }
        if (label_index < row.size()) {
            const auto marker = redaction_marker(redaction_reason::label_row);
            // Preserve only a credential name by itself. A cell such as
            // "Authorization: Bearer ..." contains the value as well.
            if (!is_sensitive_data_name(row[label_index].get<std::string>())) {
                row[label_index] = redaction_marker(label_reason.c_str());
            }
            for (size_t i = 0; i < row.size(); ++i) {
                if (i == label_index) continue;
                if (row[i].is_string() && (row[i].get<std::string>().empty() ||
                                           is_redaction_marker(row[i].get<std::string>()))) continue;
                row[i] = marker;
            }
        }
    }
}

} // namespace fairyfly::sap
