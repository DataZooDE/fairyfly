// Redaction of credentials in screen content (sensitive_data.h): labels, grids, headers, response editors, ABAP source.
// Moved out of test_screen_reader.cpp.
#include <catch2/catch_test_macros.hpp>

#include "include/screen_reader.h"
#include "include/sensitive_data.h"

#include <nlohmann/json.hpp>

#include <string>

using namespace fairyfly::sap;

static bool is_redacted(const std::string& value) { return fairyfly::sap::is_redaction_marker(value); }

TEST_CASE("Positioned report labels do not expose named credentials", "[screen][privacy][userarea]") {
    nlohmann::json elements = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,0]"}, {"type", "GuiLabel"},
         {"text", "Password"}, {"grid_row", 0}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[30,0]"}, {"type", "GuiLabel"},
         {"text", "private-report-secret"}, {"grid_row", 0}, {"grid_col", 30}},
        {{"id", "wnd[0]/usr/lbl[0,1]"}, {"type", "GuiLabel"},
         {"text", "Runtime Errors"}, {"grid_row", 1}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[30,1]"}, {"type", "GuiLabel"},
         {"text", "STRING_OFFSET_TOO_LARGE"}, {"grid_row", 1}, {"grid_col", 30}}
    });
    redact_sensitive_report_labels(elements);
    REQUIRE(is_redacted(elements[0]["text"]));
    REQUIRE(is_redacted(elements[1]["text"]));
    REQUIRE(elements[2]["text"] == "Runtime Errors");
    REQUIRE(elements[3]["text"] == "STRING_OFFSET_TOO_LARGE");
}

TEST_CASE("Gateway response header credentials are redacted", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"NAME", "VALUE"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"content-type", "application/xml"}),
            nlohmann::json::array({"Set-Cookie", "private-session-value"}),
            nlohmann::json::array({"X-CSRF-Token", "private-csrf-value"}),
            nlohmann::json::array({"Authorization", "private-auth-value"})
        })}
    };

    redact_sensitive_header_rows(table);

    REQUIRE(table["rows"][0][1] == "application/xml");
    REQUIRE(is_redacted(table["rows"][1][1]));
    REQUIRE(is_redacted(table["rows"][2][1]));
    REQUIRE(is_redacted(table["rows"][3][1]));
}

TEST_CASE("Inline credential values in grid labels are redacted", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"NAME", "VALUE"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"Authorization: Bearer private-inline", "other-value"}),
            nlohmann::json::array({"Password=private-inline", "other-value"}),
            nlohmann::json::array({"X-Session-Token", "private-value"})
        })}
    };

    redact_sensitive_header_rows(table);

    REQUIRE(is_redacted(table["rows"][0][0]));
    REQUIRE(is_redacted(table["rows"][0][1]));
    REQUIRE(is_redacted(table["rows"][1][0]));
    REQUIRE(is_redacted(table["rows"][1][1]));
    REQUIRE(table["rows"][2][0] == "X-Session-Token");
    REQUIRE(is_redacted(table["rows"][2][1]));
}

TEST_CASE("Credential columns are redacted without hiding ordinary grid data", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"USER", "PASSWORD", "API_KEY", "DESCRIPTION"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"alice", "private-password", "private-key", "Basis operator"})
        })}
    };

    redact_sensitive_header_rows(table);

    REQUIRE(table["rows"][0][0] == "alice");
    REQUIRE(is_redacted(table["rows"][0][1]));
    REQUIRE(is_redacted(table["rows"][0][2]));
    REQUIRE(table["rows"][0][3] == "Basis operator");
    REQUIRE(is_sensitive_data_name("X-CSRF-Token"));
    REQUIRE_FALSE(is_sensitive_data_name("DESCRIPTION"));
}

TEST_CASE("Header row redaction tolerates localized and missing column metadata", "[screen][privacy]") {
    nlohmann::json localized = {
        {"columns", nlohmann::json::array({"Name", "Wert", "Direction"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"Authorization", "Bearer private-token", "response"})
        })}
    };
    redact_sensitive_header_rows(localized);
    REQUIRE(localized["rows"][0][0] == "Authorization");
    REQUIRE(is_redacted(localized["rows"][0][1]));
    REQUIRE(is_redacted(localized["rows"][0][2]));

    nlohmann::json punctuated = {
        {"columns", nlohmann::json::array({"Header-Name", "Header-Value"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"Set-Cookie", "SAP_SESSIONID=private"})
        })}
    };
    redact_sensitive_header_rows(punctuated);
    REQUIRE(is_redacted(punctuated["rows"][0][1]));

    nlohmann::json no_columns = {
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"X-CSRF-Token", "private-token"})
        })}
    };
    redact_sensitive_header_rows(no_columns);
    REQUIRE_FALSE(no_columns.contains("columns"));
    REQUIRE(is_redacted(no_columns["rows"][0][1]));
}

TEST_CASE("Compound credential names are redacted in headers and columns", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"Header Name", "Header Value", "Session Token Hint"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"X-Session-Token", "private-token", "private-hint"}),
            nlohmann::json::array({"Content-Type", "application/json", "private-hint"}),
            nlohmann::json::array({"X-CSRF-Token", "Token", "private-hint"})
        })}
    };
    redact_sensitive_header_rows(table);
    REQUIRE(table["rows"][0][0] == "X-Session-Token");
    REQUIRE(is_redacted(table["rows"][0][1]));
    REQUIRE(is_redacted(table["rows"][0][2]));
    REQUIRE(table["rows"][1][1] == "application/json");
    REQUIRE(is_redacted(table["rows"][1][2]));
    REQUIRE(table["rows"][2][0] == "X-CSRF-Token");
    REQUIRE(is_redacted(table["rows"][2][1]));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "My Secret Key"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtS_TOKEN", ""));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Kennwort"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Passwort"));
}

TEST_CASE("Spanish password labels mask neutral text fields", "[screen][privacy]") {
    const std::string password_label = "Contrase\xC3\xB1" "a";
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", password_label));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Contrasena"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const std::string response = std::string("{\"") + password_label +
                                 "\":\"synthetic-spanish-marker\",\"name\":\"Flight\"}";
    const auto redacted = redact_sensitive_response_text(response);
    REQUIRE(redacted.find("synthetic-spanish-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("French password labels mask neutral text fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Mot de passe"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Nouveau mot de passe"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const auto redacted = redact_sensitive_response_text(
        R"({"Mot de passe":"synthetic-french-marker","name":"Flight"})");
    REQUIRE(redacted.find("synthetic-french-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("Portuguese password labels mask neutral text fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Senha"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Nova senha"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const auto redacted = redact_sensitive_response_text(
        R"({"Senha":"synthetic-portuguese-marker","name":"Flight"})");
    REQUIRE(redacted.find("synthetic-portuguese-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("Dutch password labels mask neutral text fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Wachtwoord"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Nieuw wachtwoord"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const auto redacted = redact_sensitive_response_text(
        R"({"Wachtwoord":"synthetic-dutch-marker","name":"Flight"})");
    REQUIRE(redacted.find("synthetic-dutch-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("Unicode password labels mask neutral text fields", "[screen][privacy]") {
    const std::vector<std::string> labels = {
        "\xE5\xAF\x86\xE7\xA0\x81", // Chinese
        "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89", // Japanese
        "\xD0\x9F\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C", // Russian
        "\xC5\x9E" "ifre", // Turkish
        "Has\xC5\x82" "o" // Polish
    };
    for (size_t index = 0; index < labels.size(); ++index) {
        INFO("locale index " << index);
        REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", labels[index]));
        const std::string marker = "synthetic-unicode-marker-" + std::to_string(index);
        const auto redacted = redact_sensitive_response_text(
            "{\"" + labels[index] + "\":\"" + marker + "\",\"name\":\"Flight\"}");
        REQUIRE(redacted.find(marker) == std::string::npos);
    }
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
}

TEST_CASE("Truncated JSON masks Unicode-escaped password keys", "[screen][privacy]") {
    const auto chinese = redact_sensitive_response_text(
        R"({"\u5bc6\u7801":"synthetic-escaped-chinese-marker",)");
    REQUIRE(chinese.find("synthetic-escaped-chinese-marker") == std::string::npos);
    const auto japanese = redact_sensitive_response_text(
        R"({"\u30d1\u30b9\u30ef\u30fc\u30c9":"synthetic-escaped-japanese-marker",)");
    REQUIRE(japanese.find("synthetic-escaped-japanese-marker") == std::string::npos);
    const auto unquoted = redact_sensitive_response_text(
        R"({\u5bc6\u7801:synthetic-unquoted-marker,)");
    REQUIRE(unquoted.find("synthetic-unquoted-marker") == std::string::npos);
    const auto ordinary = redact_sensitive_response_text(
        R"({"\u0043ontent-Type":"application/json",)");
    REQUIRE(ordinary.find("application/json") != std::string::npos);
}

TEST_CASE("XML numeric Unicode password names are masked", "[screen][privacy]") {
    const auto hex = redact_sensitive_response_text(
        "<Name>&#x5BC6;&#x7801;</Name><Value>synthetic-xml-unicode-hex</Value>");
    REQUIRE(hex.find("synthetic-xml-unicode-hex") == std::string::npos);
    const auto decimal = redact_sensitive_response_text(
        "<Name>&#23494;&#30721;</Name><Value>synthetic-xml-unicode-decimal</Value>");
    REQUIRE(decimal.find("synthetic-xml-unicode-decimal") == std::string::npos);
    const auto ordinary = redact_sensitive_response_text(
        "<Name>Content-Type</Name><Value>application/json</Value>");
    REQUIRE(ordinary.find("application/json") != std::string::npos);
}

TEST_CASE("Response editor masks JSON credentials and HTTP auth headers", "[screen][privacy]") {
    const std::string body = R"({"data":{"id":7,"access_token":"private-token","name":"Flight"},"items":[{"clientSecret":"private-secret"}]})";
    const auto redacted = redact_sensitive_response_text(body);
    REQUIRE(redacted.find("private-token") == std::string::npos);
    REQUIRE(redacted.find("private-secret") == std::string::npos);
    REQUIRE(redacted.find("\"name\": \"Flight\"") != std::string::npos);
    REQUIRE(redacted.find("\"id\": 7") != std::string::npos);

    const std::string headers = "HTTP/1.1 200 OK\r\nAuthorization: Bearer private-auth\r\n"
                                "Set-Cookie: private-cookie\r\nContent-Type: application/json\r\n";
    const auto redacted_headers = redact_sensitive_response_text(headers);
    REQUIRE(redacted_headers.find("private-auth") == std::string::npos);
    REQUIRE(redacted_headers.find("private-cookie") == std::string::npos);
    REQUIRE(redacted_headers.find("Content-Type: application/json") != std::string::npos);
}

TEST_CASE("Response editor suppresses recognizable XML and broken JSON secrets", "[screen][privacy]") {
    const std::string xml = "<entry><Name>Flight</Name><d:AccessToken>private-xml</d:AccessToken></entry>";
    REQUIRE(redact_sensitive_response_text(xml) == "[REDACTED]");
    const std::string xml_attribute = "<entry clientSecret='private-attribute'><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attribute) == "[REDACTED]");
    const std::string broken_json = R"({"name":"Flight","access_token":"private-broken")";
    REQUIRE(redact_sensitive_response_text(broken_json) == "[REDACTED]");
    const std::string broken_named_json =
        R"({"headers":[{"Name":"Authorization","Value":"private-auth")";
    REQUIRE(redact_sensitive_response_text(broken_named_json) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text("<entry><Name>Flight</Name></entry>") ==
            "<entry><Name>Flight</Name></entry>");
    REQUIRE(redact_sensitive_response_text(R"({"name":"Flight")") ==
            R"({"name":"Flight")");
}

TEST_CASE("Response editor suppresses XML values in Unicode element tags and attributes", "[screen][privacy]") {
    const std::string xml_ko = "<entry><Name>Flight</Name><비밀번호>SYNTH_XML_SECRET_KO</비밀번호></entry>";
    REQUIRE(redact_sensitive_response_text(xml_ko) == "[REDACTED]");

    const std::string xml_zh = "<entry><Name>Flight</Name><密码>SYNTH_XML_SECRET_ZH</密码></entry>";
    REQUIRE(redact_sensitive_response_text(xml_zh) == "[REDACTED]");

    const std::string xml_ru = "<entry><Name>Flight</Name><Пароль>SYNTH_XML_SECRET_RU</Пароль></entry>";
    REQUIRE(redact_sensitive_response_text(xml_ru) == "[REDACTED]");

    const std::string xml_sv = "<entry><Name>Flight</Name><Lösenord>SYNTH_XML_SECRET_SV</Lösenord></entry>";
    REQUIRE(redact_sensitive_response_text(xml_sv) == "[REDACTED]");

    const std::string xml_de = "<entry><Name>Flight</Name><Passwort>SYNTH_XML_SECRET_DE</Passwort></entry>";
    REQUIRE(redact_sensitive_response_text(xml_de) == "[REDACTED]");

    const std::string xml_ns = "<entry><Name>Flight</Name><d:비밀번호>SYNTH_XML_SECRET_NAMESPACED</d:비밀번호></entry>";
    REQUIRE(redact_sensitive_response_text(xml_ns) == "[REDACTED]");

    const std::string xml_attr_ko = "<entry 비밀번호=\"SYNTH_XML_ATTR_SECRET_KO\"><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attr_ko) == "[REDACTED]");

    const std::string xml_attr_zh = "<entry 密码=\"SYNTH_XML_ATTR_SECRET_ZH\"><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attr_zh) == "[REDACTED]");

    const std::string xml_attr_sv = "<entry Lösenord=\"SYNTH_XML_ATTR_SECRET_SV\"><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attr_sv) == "[REDACTED]");

    const std::string ordinary = "<entry><Name>Content-Type</Name><Value>application/xml</Value></entry>";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);
}

TEST_CASE("Extended multilingual password labels mask neutral input fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "비밀번호"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "암호"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Lösenord"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Passord"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Adgangskode"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Parola d'ordine"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Heslo"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Jelszó"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Salasana"));
    REQUIRE(is_sensitive_data_name("Passwort"));
    REQUIRE(is_sensitive_data_name("Kennwort"));
    REQUIRE(!is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Description"));
}

TEST_CASE("Response editor suppresses XML values paired with credential names", "[screen][privacy]") {
    const std::string header =
        "<headers><header><Name>Authorization</Name><Value>FF_XML_SECRET_01</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(header) == "[REDACTED]");

    const std::string field =
        "<fields><field><Key>client_secret</Key><Value>FF_XML_SECRET_02</Value></field></fields>";
    REQUIRE(redact_sensitive_response_text(field) == "[REDACTED]");

    const std::string cdata =
        "<headers><header><Name><![CDATA[Authorization]]></Name><Value>FF_XML_SECRET_03</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(cdata) == "[REDACTED]");

    const std::string attributes =
        "<headers><header Name=\"Authorization\" Value=\"FF_XML_SECRET_04\"/></headers>";
    REQUIRE(redact_sensitive_response_text(attributes) == "[REDACTED]");

    const std::string unquoted_attributes =
        "<headers><header Name=Authorization Value=FF_XML_SECRET_05/></headers>";
    REQUIRE(redact_sensitive_response_text(unquoted_attributes) == "[REDACTED]");

    const std::string encoded_element =
        "<headers><header><Name>Authoriz&#97;tion</Name><Value>FF_XML_SECRET_06</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(encoded_element) == "[REDACTED]");

    const std::string padded_entity =
        "<headers><header><Name>Authoriz&#00000000000000000097;tion</Name><Value>FF_XML_SECRET_08</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(padded_entity) == "[REDACTED]");

    const std::string encoded_attribute =
        "<headers><header Name=\"client&#x5F;secret\" Value=\"FF_XML_SECRET_07\"/></headers>";
    REQUIRE(redact_sensitive_response_text(encoded_attribute) == "[REDACTED]");

    const std::string truncated =
        "<headers><header><Name>Authorization<Value>FF_XML_SECRET_09</Value>";
    REQUIRE(redact_sensitive_response_text(truncated) == "[REDACTED]");

    const std::string bom_prefixed =
        "\xEF\xBB\xBF<headers><header><Name>Authorization</Name><Value>FF_XML_SECRET_10</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(bom_prefixed) == "[REDACTED]");

    const std::string bom_json = "\xEF\xBB\xBF{\"access_token\":\"FF_XML_SECRET_11\"}";
    REQUIRE(redact_sensitive_response_text(bom_json).find("FF_XML_SECRET_11") == std::string::npos);

    const std::string ordinary =
        "<headers><header><Name>Content-Type</Name><Value>application/json</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);

    const std::string ordinary_attributes =
        "<headers><header Name=Content-Type Value=application/json/></headers>";
    REQUIRE(redact_sensitive_response_text(ordinary_attributes) == ordinary_attributes);

    const std::string ordinary_encoded =
        "<headers><header><Name>Content&#45;Type</Name><Value>application/json</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(ordinary_encoded) == ordinary_encoded);

    const std::string ordinary_truncated =
        "<headers><header><Name>Content-Type<Value>application/json</Value>";
    REQUIRE(redact_sensitive_response_text(ordinary_truncated) == ordinary_truncated);
}

TEST_CASE("Response editor masks generic values paired with credential names", "[screen][privacy]") {
    const auto result = redact_sensitive_response_text(
        R"({"headers":[{"Name":"Authorization","Value":"private-auth"},{"Name":"Content-Type","Value":"application/json"}],"data":{"value":"ordinary"}})");
    REQUIRE(result.find("private-auth") == std::string::npos);
    REQUIRE(result.find("\"Name\": \"Authorization\"") != std::string::npos);
    REQUIRE(result.find("application/json") != std::string::npos);
    REQUIRE(result.find("ordinary") != std::string::npos);
}

TEST_CASE("Response editor masks credential values in JSON header pairs", "[screen][privacy]") {
    const auto result = redact_sensitive_response_text(
        R"({"headers":[["Authorization","FF_JSON_PAIR_SECRET"],["Content-Type","application/json"]],"status":"ready"})");
    REQUIRE(result.find("FF_JSON_PAIR_SECRET") == std::string::npos);
    REQUIRE(result.find("Authorization") != std::string::npos);
    REQUIRE(result.find("application/json") != std::string::npos);
    REQUIRE(result.find("ready") != std::string::npos);

    const std::string malformed =
        "{headers:[[Authorization,FF_JSON_PAIR_BROKEN],[Content-Type,application/json]]}";
    REQUIRE(redact_sensitive_response_text(malformed) == "[REDACTED]");
    const std::string ordinary_malformed = "{headers:[[Content-Type,application/json]]}";
    REQUIRE(redact_sensitive_response_text(ordinary_malformed) == ordinary_malformed);
}

TEST_CASE("Response editor suppresses unquoted object pairs from GUI editors", "[screen][privacy]") {
    const std::string live_shape =
        "{FieldName:Authorization,Value:FFLY_FIELDNAME_PAIR_260927_X7}";
    REQUIRE(redact_sensitive_response_text(live_shape) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        "{HeaderName:client_secret,HeaderValue:FFLY_HEADER_PAIR_260927}") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        "{access_token:FFLY_DIRECT_KEY_260927}") == "[REDACTED]");
    const std::string ordinary = "{FieldName:Content-Type,Value:application/json}";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);
}

TEST_CASE("Response editor suppresses escaped credential names in malformed object text", "[screen][privacy]") {
    const std::string escaped_name =
        R"({FieldName:Authoriz\u0061tion,Value:FFLY_ESCAPED_PAIR_260927})";
    REQUIRE(redact_sensitive_response_text(escaped_name) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({access_tok\u0065n:FFLY_ESCAPED_KEY_260927})") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({"access_tok\u0065n":"FFLY_QUOTED_KEY_260927")") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({FieldN\u0061me:Authorization,Value:FFLY_ESCAPED_LABEL_260927})") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({"FieldN\u0061me":"Authorization","Value":"FFLY_QUOTED_LABEL_260927")") == "[REDACTED]");
    const std::string ordinary =
        R"({FieldName:Content-\u0054ype,Value:application/json})";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);
}

TEST_CASE("Response editor masks named credentials in plain text", "[screen][privacy]") {
    const std::string body = "status: ready\r\naccess_token=private-token\r\n"
                             "  client-secret: private-secret\nAuthorization: Bearer private-auth\n"
                             "Content-Type: text/plain\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("private-token") == std::string::npos);
    REQUIRE(result.find("private-secret") == std::string::npos);
    REQUIRE(result.find("private-auth") == std::string::npos);
    REQUIRE(result.find("status: ready") != std::string::npos);
    REQUIRE(result.find("Content-Type: text/plain") != std::string::npos);
}

TEST_CASE("Response editor masks credential headers on CR-only GUI lines", "[screen][privacy]") {
    const std::string body =
        "HTTP/1.1 200 OK\rAuthorization: Bearer FFLY_CR_ONLY_AUTH_260927\r"
        "Content-Type: application/json\rSet-Cookie: FFLY_CR_ONLY_COOKIE_260927";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FFLY_CR_ONLY_AUTH_260927") == std::string::npos);
    REQUIRE(result.find("FFLY_CR_ONLY_COOKIE_260927") == std::string::npos);
    REQUIRE(result.find("Content-Type: application/json") != std::string::npos);
}

TEST_CASE("Response editor suppresses plain-text values paired with credential names", "[screen][privacy]") {
    const std::string credential_pair =
        "Name: Authorization\rValue: FFLY_PLAIN_PAIR_260927";
    REQUIRE(redact_sensitive_response_text(credential_pair) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        "HeaderName=client_secret\r\nHeaderValue=FFLY_PLAIN_PAIR_CRLF") == "[REDACTED]");

    const std::string ordinary_pair =
        "Name: Content-Type\r\nValue: application/json";
    REQUIRE(redact_sensitive_response_text(ordinary_pair) == ordinary_pair);
}

TEST_CASE("Response editor masks credential query parameters in URLs", "[screen][privacy]") {
    const std::string body =
        "GET /sap/opu/odata?access_token=FF_QUERY_81&$top=1 HTTP/1.1\n"
        "Location: https://bigfox.example/path?client_secret=FF_QUERY_82&mode=read\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_QUERY_81") == std::string::npos);
    REQUIRE(result.find("FF_QUERY_82") == std::string::npos);
    REQUIRE(result.find("$top=1") != std::string::npos);
    REQUIRE(result.find("mode=read") != std::string::npos);
}

TEST_CASE("Response editor masks credentials in URL fragments", "[screen][privacy]") {
    const std::string body =
        "Location: https://bigfox.example/callback#access_token=FF_FRAGMENT_83&state=ready\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_FRAGMENT_83") == std::string::npos);
    REQUIRE(result.find("state=ready") != std::string::npos);
}

TEST_CASE("Response editor masks percent-encoded credential parameter names", "[screen][privacy]") {
    const std::string body =
        "GET /sap/opu/odata?access%5Ftoken=FF_ENCODED_01&$top=1 HTTP/1.1\n"
        "Location: https://bigfox.example/callback#client%5Fsecret=FF_ENCODED_02&state=ready\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_ENCODED_01") == std::string::npos);
    REQUIRE(result.find("FF_ENCODED_02") == std::string::npos);
    REQUIRE(result.find("$top=1") != std::string::npos);
    REQUIRE(result.find("state=ready") != std::string::npos);

    const auto json_result = redact_sensitive_response_text(
        R"({"url":"https://bigfox.example/?%61ccess_token=FF_ENCODED_03&mode=read"})");
    REQUIRE(json_result.find("FF_ENCODED_03") == std::string::npos);
    REQUIRE(json_result.find("mode=read") != std::string::npos);
}

TEST_CASE("Response editor masks semicolon-delimited credential URL parameters", "[screen][privacy]") {
    const std::string body =
        "GET /sap/opu/odata?$top=1;access_token=FF_SEMICOLON_01&mode=read HTTP/1.1\n"
        "Location: https://bigfox.example/callback#state=ready;client_secret=FF_SEMICOLON_02\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_SEMICOLON_01") == std::string::npos);
    REQUIRE(result.find("FF_SEMICOLON_02") == std::string::npos);
    REQUIRE(result.find("$top=1") != std::string::npos);
    REQUIRE(result.find("mode=read") != std::string::npos);
    REQUIRE(result.find("state=ready") != std::string::npos);
}

TEST_CASE("ABAP source masks credential statements without shifting line numbers", "[screen][privacy][editor]") {
    const std::string source =
        "DATA(lv_password) = 'FF_PAYLOAD_42'.\n"
        "DATA(lv_token) =\n"
        "  'FF_PAYLOAD_43'.\n"
        "DATA(lv_value) = 'FF_PAYLOAD_44'\n"
        "  && lv_client_secret.\n"
        "WRITE 'ordinary output'.";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("FF_PAYLOAD_42") == std::string::npos);
    REQUIRE(redacted.find("FF_PAYLOAD_43") == std::string::npos);
    REQUIRE(redacted.find("FF_PAYLOAD_44") == std::string::npos);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("ABAP source keeps template and backtick periods inside credential statements", "[screen][privacy][editor]") {
    const std::string source =
        "DATA(lv_value) = |FF_TEMPLATE.51|\n"
        "  && lv_client_secret.\n"
        "DATA(lv_other) = `FF_BACKTICK.52`\n"
        "  && lv_password.\n"
        "WRITE 'ordinary output'.";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("FF_TEMPLATE.51") == std::string::npos);
    REQUIRE(redacted.find("FF_BACKTICK.52") == std::string::npos);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("ABAP source does not release an unfinished second statement", "[screen][privacy][editor]") {
    const std::string source =
        "WRITE 'ready'. DATA(lv_value) = |FF_SECOND.63|\n"
        "  && lv_password.\n"
        "WRITE 'ordinary output'.";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("FF_SECOND.63") == std::string::npos);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("ABAP source suppresses an unfinished editor range", "[screen][privacy][editor]") {
    // The credential name may be on line 201, beyond the bounded editor read.
    const std::string source =
        "WRITE 'ordinary output'.\n"
        "DATA(lv_value) = 'FF_BOUNDARY_64' &&\n";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(redacted.find("FF_BOUNDARY_64") == std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}
