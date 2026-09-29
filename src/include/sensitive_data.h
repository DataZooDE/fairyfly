#pragma once

#include <algorithm>
#include <cctype>
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

inline bool is_sensitive_input_field(const std::string& type, const std::string& id,
                                     const std::string& label) {
    if (type == "GuiPasswordField") return true;
    if (type != "GuiTextField" && type != "GuiCTextField" &&
        type != "GuiComboBox" && type != "GuiComboBoxControl") return false;

    const auto last_separator = id.find_last_of('/');
    const std::string field_name = last_separator == std::string::npos
        ? id : id.substr(last_separator + 1);
    if (contains_sensitive_data_name(field_name)) return true;
    // SAP authorization objects and groups are access-control metadata, not
    // credential values. Keep technical field names authoritative.
    const auto normalized_label = normalize_sensitive_name(label);
    if (normalized_label == "authorizationobject" ||
        normalized_label == "authorizationgroup") return false;
    return contains_sensitive_data_name(label);
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
inline void redact_sensitive_header_rows(nlohmann::json& table) {
    if (!table.is_object()) return;
    auto rows_it = table.find("rows");
    if (rows_it == table.end() || !rows_it->is_array()) return;

    int name_column = -1;
    std::vector<size_t> sensitive_columns;
    auto columns_it = table.find("columns");
    if (columns_it != table.end() && columns_it->is_array()) {
        const auto& columns = *columns_it;
        for (size_t i = 0; i < columns.size(); ++i) {
            if (!columns[i].is_string()) continue;
            const auto title = columns[i].get<std::string>();
            const auto name = normalize_sensitive_name(title);
            if (name == "name" || name == "headername") name_column = static_cast<int>(i);
            if (contains_sensitive_data_name(title)) sensitive_columns.push_back(i);
        }
    }

    for (auto& row : *rows_it) {
        if (!row.is_array()) continue;
        for (size_t index : sensitive_columns) {
            if (index < row.size()) row[index] = "[REDACTED]";
        }

        size_t label_index = row.size();
        if (name_column >= 0 && static_cast<size_t>(name_column) < row.size() &&
            row[name_column].is_string() &&
            contains_sensitive_data_name(row[name_column].get<std::string>())) {
            label_index = static_cast<size_t>(name_column);
        } else {
            // Some SAP grids expose no column titles, or localize the value title.
            for (size_t i = 0; i < row.size(); ++i) {
                if (row[i].is_string() &&
                    contains_sensitive_data_name(row[i].get<std::string>())) {
                    label_index = i;
                    break;
                }
            }
        }
        if (label_index < row.size()) {
            // Preserve only a credential name by itself. A cell such as
            // "Authorization: Bearer ..." contains the value as well.
            if (!is_sensitive_data_name(row[label_index].get<std::string>())) {
                row[label_index] = "[REDACTED]";
            }
            for (size_t i = 0; i < row.size(); ++i) {
                if (i != label_index) row[i] = "[REDACTED]";
            }
        }
    }
}

} // namespace fairyfly::sap
