#include <catch2/catch_test_macros.hpp>
#include "include/display_text_policy.h"
#include "include/sensitive_data.h"
#include <map>

using namespace fairyfly::sap;

namespace {
// Scripted inputs: counts the lazy reads so the tests can prove what was (not) touched.
struct Script {
    std::string type = "GuiTextField";
    std::string id = "wnd[0]/usr/txtFIELD";
    std::string label;
    std::optional<bool> changeable = true;
    std::optional<std::string> displayed = "shown";
    std::string text = "plain";
    // Sibling texts by leaf; a missing leaf is not visited. `probe_ok` = false is "unverified".
    std::map<std::string, std::string> siblings;
    bool probe_ok = true;
    bool has_probe = true;

    int id_reads = 0, label_reads = 0, changeable_reads = 0, displayed_reads = 0, text_reads = 0;
    int probe_calls = 0;
    SiblingQuery last_query;

    DisplayTextInputs inputs() {
        DisplayTextInputs in;
        in.type = type;
        in.id = [this] { ++id_reads; return id; };
        in.label = [this] { ++label_reads; return label; };
        in.changeable = [this] { ++changeable_reads; return changeable; };
        in.displayed_text = [this] { ++displayed_reads; return displayed; };
        in.text = [this] { ++text_reads; return text; };
        if (has_probe) {
            in.sibling_probe = [this](const SiblingQuery& query, const SiblingVisitor& visit) {
                ++probe_calls;
                last_query = query;
                if (!probe_ok) return false;
                for (size_t index = 0; index < query.leaves.size(); ++index) {
                    const auto it = siblings.find(query.leaves[index]);
                    if (it != siblings.end() && !visit(index, it->second)) return true;
                }
                return true;
            };
        }
        return in;
    }
};

std::string marker(const char* reason) { return redaction_marker(reason); }
}  // namespace

TEST_CASE("display text policy: password fields are never read", "[privacy][display-text]") {
    Script s;
    s.type = "GuiPasswordField";
    s.id = "wnd[0]/usr/pwdRSYST-BCODE";
    CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::password_field));
    CHECK(s.id_reads == 0);
    CHECK(s.displayed_reads == 0);
    CHECK(s.text_reads == 0);
}

TEST_CASE("display text policy: credential-named fields", "[privacy][display-text]") {
    Script s;
    SECTION("changeable PASSWORD field is redacted without reading its value") {
        s.id = "wnd[0]/usr/txtPASSWORD";
        s.changeable = true;
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::password_name));
        CHECK(s.text_reads == 0);
        CHECK(s.displayed_reads == 0);
    }
    SECTION("a neutral id is redacted by its label") {
        s.id = "wnd[0]/usr/txtNEUTRAL";
        s.label = "Password";
        CHECK(is_redaction_marker(resolve_display_text(s.inputs())));
        CHECK(s.text_reads == 0);
    }
    SECTION("changeability is read only for text and combo types") {
        s.type = "GuiLabel";
        s.id = "wnd[0]/usr/lblPASSWORD";
        CHECK(resolve_display_text(s.inputs()) == "shown");
        CHECK(s.changeable_reads == 0);
        CHECK(s.label_reads == 0);
    }
}

TEST_CASE("display text policy: display-only state labels are exempt", "[privacy][display-text]") {
    Script s;
    s.id = "wnd[0]/usr/txtPASSWORD_EXT_PWD_STATE";
    s.changeable = false;
    s.displayed = "Password set";
    SECTION("known display-only field with a state value is shown") {
        CHECK(resolve_display_text(s.inputs()) == "Password set");
    }
    SECTION("DisplayedText empty: the state value comes from Text") {
        s.displayed = "";
        s.text = "locked";
        CHECK(resolve_display_text(s.inputs()) == "locked");
    }
    SECTION("changeable field stays redacted") {
        s.changeable = true;
        CHECK(is_redaction_marker(resolve_display_text(s.inputs())));
        CHECK(s.text_reads == 0);
    }
    SECTION("unknown changeability stays redacted") {
        s.changeable = std::nullopt;
        CHECK(is_redaction_marker(resolve_display_text(s.inputs())));
    }
    SECTION("a secret-looking value is not a state label") {
        s.displayed = "aB3$xY9!kLm2Qw";
        CHECK(is_redaction_marker(resolve_display_text(s.inputs())));
    }
    SECTION("unavailable DisplayedText fails closed") {
        s.displayed = std::nullopt;
        CHECK(is_redaction_marker(resolve_display_text(s.inputs())));
    }
}

TEST_CASE("display text policy: VALUE field follows its NAME sibling", "[privacy][display-text]") {
    Script s;
    s.id = "wnd[1]/usr/txtVALUE";
    s.displayed = "application/json";

    SECTION("benign sibling name lets the value through") {
        s.siblings["txtNAME"] = "Content-Type";
        CHECK(resolve_display_text(s.inputs()) == "application/json");
        CHECK(s.probe_calls == 1);
        CHECK(s.last_query.row.empty());
        CHECK(s.last_query.leaves[0] == "txtNAME");
        CHECK(s.last_query.leaves[1] == "txtKEY");
        CHECK(s.last_query.leaves[2] == "txtFIELDNAME");
        CHECK(s.last_query.leaves[3] == "txtHEADERNAME");
    }
    SECTION("no sibling at all is a verified absence") {
        CHECK(resolve_display_text(s.inputs()) == "application/json");
    }
    SECTION("credential sibling hides the value") {
        s.siblings["txtNAME"] = "Authorization";
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::paired_name));
        CHECK(s.text_reads == 0);
    }
    SECTION("credential KEY sibling hides the value even after a benign NAME") {
        s.siblings["txtNAME"] = "Content-Type";
        s.siblings["txtKEY"] = "X-Session-Token";
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::paired_name));
    }
    SECTION("empty sibling name hides the value") {
        s.siblings["txtNAME"] = "";
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::paired_name));
    }
    SECTION("an already redacted sibling name hides the value") {
        s.siblings["txtNAME"] = marker(redaction_reason::password_field);
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::paired_name));
    }
    SECTION("missing parent (probe cannot observe) is the unverified marker") {
        s.probe_ok = false;
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::unverified));
        CHECK(s.displayed_reads == 0);
        CHECK(s.text_reads == 0);
    }
    SECTION("no probe at all is unverified") {
        s.has_probe = false;
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::unverified));
    }
    SECTION("ctxt fields use the same sibling check") {
        s.type = "GuiCTextField";
        s.id = "wnd[1]/usr/ctxtOPT_VALUE";
        s.siblings["ctxtOPT_NAME"] = "Password";
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::paired_name));
        CHECK(s.last_query.leaves[0] == "ctxtOPT_NAME");
    }
    SECTION("table control rows are passed to the probe") {
        s.id = "wnd[1]/usr/tblX/txtVALUE[1,3]";
        CHECK(resolve_display_text(s.inputs()) == "application/json");
        CHECK(s.last_query.row == "3");
    }
    SECTION("fields not ending in VALUE never probe") {
        s.id = "wnd[1]/usr/txtVALUE2";
        CHECK(resolve_display_text(s.inputs()) == "application/json");
        CHECK(s.probe_calls == 0);
    }
}

TEST_CASE("display text policy: the gateway header value needs its NAME sibling", "[privacy][display-text]") {
    Script s;
    s.id = "wnd[1]/usr/txtIP_HEADER_VALUE";
    s.displayed = "application/json";
    SECTION("verified benign NAME") {
        s.siblings["txtIP_HEADER_NAME"] = "Content-Type";
        CHECK(resolve_display_text(s.inputs()) == "application/json");
    }
    SECTION("NAME sibling absent fails closed") {
        CHECK(resolve_display_text(s.inputs()) == marker(redaction_reason::unverified));
    }
}

TEST_CASE("display text policy: structured text is redacted", "[privacy][display-text]") {
    Script s;
    s.type = "GuiLabel";
    s.id = "wnd[0]/usr/lbl[1,1]";
    SECTION("name=value text naming a credential") {
        s.displayed = "client_secret=abcdef";
        const auto result = resolve_display_text(s.inputs());
        CHECK(result == redact_sensitive_response_text("client_secret=abcdef"));
        CHECK(result.find("abcdef") == std::string::npos);
    }
    SECTION("unstructured text naming a credential is left alone") {
        s.displayed = "Password administration";
        CHECK(resolve_display_text(s.inputs()) == "Password administration");
    }
    SECTION("text editors and shells are always run through the response redactor") {
        s.type = "GuiTextedit";
        s.displayed = "token: abcdef";
        CHECK(resolve_display_text(s.inputs()) == redact_sensitive_response_text("token: abcdef"));
    }
    SECTION("the Text fallback is filtered too") {
        s.displayed = "";
        s.text = "api_key: abcdef";
        const auto result = resolve_display_text(s.inputs());
        CHECK(result.find("abcdef") == std::string::npos);
    }
}

TEST_CASE("display text policy: DisplayedText then Text", "[display-text]") {
    Script s;
    s.type = "GuiLabel";
    SECTION("DisplayedText wins and Text is not read") {
        CHECK(resolve_display_text(s.inputs()) == "shown");
        CHECK(s.text_reads == 0);
    }
    SECTION("empty DisplayedText falls back to Text") {
        s.displayed = "";
        CHECK(resolve_display_text(s.inputs()) == "plain");
        CHECK(s.displayed_reads == 1);
        CHECK(s.text_reads == 1);
    }
    SECTION("unavailable DisplayedText falls back to Text") {
        s.displayed = std::nullopt;
        CHECK(resolve_display_text(s.inputs()) == "plain");
    }
}

TEST_CASE("split_field_index keeps the table row", "[display-text]") {
    CHECK(split_field_index("txtVALUE[1,3]") == std::pair<std::string, std::string>{"txtVALUE", "3"});
    CHECK(split_field_index("txtVALUE") == std::pair<std::string, std::string>{"txtVALUE", ""});
    CHECK(split_field_index("txtVALUE[1,x]") == std::pair<std::string, std::string>{"txtVALUE[1,x]", ""});
}
