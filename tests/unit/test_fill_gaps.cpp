// Issue #158: element fill gaps seen live on SE38 (A4H/Bigfox, SAP GUI 8.00).
//  - A combo box or radio button choice runs a server round trip: the object fairyfly wrote to goes stale (every call
//    answers E_NOTIMPL), so the read-back must come from the element found again by its id, and a combo box whose key
//    did not change is an error, not a success.
//  - The ABAP editor (GuiShell, SubType AbapEditor) is written with SelectAll + ReplaceSelection; in display mode it
//    still accepts text (Changeable is true there), so the window's Save button tells whether the source is editable.
#include <catch2/catch_test_macros.hpp>

#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/com/wrapper.h"
#include "include/com_automation_engine.h"
#include "support/fake_dispatch.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using namespace fairyfly::test;
using json = nlohmann::json;

namespace {

constexpr const wchar_t* kSession = L"/app/con[0]/ses[0]";

struct FillScene {
    ScopedDispatchCacheReset cache_reset;
    std::vector<FakeNode*> nodes;
    FakeNode *app, *connections, *connection, *sessions, *session, *window, *save;

    FakeNode* make(const wchar_t* type, const std::wstring& id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        node->bools[L"Changeable"] = true;
        nodes.push_back(node);
        return node;
    }
    /// Registers a node under its full id and the window-relative one the commands pass on.
    void place(const std::wstring& full, FakeNode* node) {
        session->find_by_id[full] = node;
        session->find_by_id[full.substr(std::wcslen(kSession) + 1)] = node;
    }
    explicit FillScene(const wchar_t* window_id = L"/app/con[0]/ses[0]/wnd[0]") {
        app = make(L"GuiApplication", L"/app");
        connections = make(L"GuiComponentCollection", L"");
        connection = make(L"GuiConnection", L"/app/con[0]");
        sessions = make(L"GuiComponentCollection", L"");
        session = make(L"GuiSession", kSession);
        window = make(L"GuiMainWindow", window_id);
        save = make(L"GuiButton", std::wstring(window_id) + L"/tbar[0]/btn[11]");
        app->dispatches[L"Connections"] = connections;
        connections->items = {connection};
        connection->dispatches[L"Children"] = sessions;
        connection->bools[L"DisabledByServer"] = false;
        sessions->items = {session};
        session->dispatches[L"ActiveWindow"] = window;
        place(window_id, window);
        place(std::wstring(window_id) + L"/tbar[0]/btn[11]", save);
    }
    ~FillScene() {
        for (auto* node : nodes) node->Release();
    }
    std::unique_ptr<ComAutomationEngine> engine() {
        app->AddRef();
        return std::make_unique<ComAutomationEngine>(std::make_shared<ComGuiApplication>(IDispatchPtr(app, false)));
    }
};

constexpr const wchar_t* kCombo = L"/app/con[0]/ses[0]/wnd[1]/usr/cmbTRDIR-SUBC";

FakeNode* combo_node(FillScene& scene, const wchar_t* key, const wchar_t* value) {
    FakeNode* combo = scene.make(L"GuiComboBox", kCombo);
    combo->strings[L"Key"] = key;
    combo->strings[L"Value"] = value;
    combo->strings[L"Text"] = value;
    FakeNode* entries = scene.make(L"GuiCollection", L"");
    entries->enumerable = true;
    for (auto [k, v] : {std::pair{L"1", L"Executable program"}, std::pair{L"I", L"INCLUDE program"}, std::pair{L" ", L" "}}) {
        FakeNode* entry = scene.make(L"GuiComboBoxEntry", L"");
        entry->strings[L"Key"] = k;
        entry->strings[L"Value"] = v;
        entries->items.push_back(entry);
    }
    combo->dispatches[L"Entries"] = entries;
    return combo;
}

/// The SE38 program-attributes popup: writing the Key runs a round trip, the written object goes stale and SAP GUI
/// serves a new object under the same id that shows `then_key`.
struct ComboRoundTrip {
    FakeNode* written;
    FakeNode* fresh;
    ComboRoundTrip(FillScene& scene, const wchar_t* then_key, const wchar_t* then_value) {
        written = combo_node(scene, L" ", L" ");
        fresh = combo_node(scene, then_key, then_value);
        scene.place(kCombo, written);
        written->on_put = [this, &scene](const std::wstring& name, const std::wstring&) {
            if (name != L"Key") return;
            written->stale = true;
            scene.place(kCombo, fresh);
        };
    }
};

}  // namespace

TEST_CASE("fill gaps: a combo box choice is read back from the element after the round trip", "[fill][issue158]") {
    FillScene scene(L"/app/con[0]/ses[0]/wnd[1]");
    ComboRoundTrip combo(scene, L"1", L"Executable program");
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[1]/usr/cmbTRDIR-SUBC"), "Executable program");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data["key"] == "1");
    CHECK(r.data["display_value"] == "Executable program");
    CHECK(r.data["value"] == "Executable program");
}

TEST_CASE("fill gaps: a combo box whose key did not change is an error, not a success", "[fill][issue158]") {
    FillScene scene(L"/app/con[0]/ses[0]/wnd[1]");
    ComboRoundTrip combo(scene, L" ", L" ");   // SAP kept the old (empty) choice
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[1]/usr/cmbTRDIR-SUBC"), "1");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "FILL_NOT_APPLIED");
    CHECK(r.error["written_key"] == "1");
    CHECK(r.error["read_back_key"] == "");
    CHECK(r.error["element"] == "wnd[1]/usr/cmbTRDIR-SUBC");
}

TEST_CASE("fill gaps: a radio button reports selected and its caption after the round trip", "[fill][issue158]") {
    FillScene scene(L"/app/con[0]/ses[0]/wnd[1]");
    const std::wstring id = L"/app/con[0]/ses[0]/wnd[1]/usr/rad*SEUCUSTOM-PCMODE";
    FakeNode* written = scene.make(L"GuiRadioButton", id);
    written->bools[L"Selected"] = false;
    written->strings[L"Text"] = L"Text-Based Editor";
    FakeNode* fresh = scene.make(L"GuiRadioButton", id);
    fresh->bools[L"Selected"] = true;
    fresh->strings[L"Text"] = L"Text-Based Editor";
    scene.place(id, written);
    written->on_select = [&] {
        written->stale = true;
        scene.place(id, fresh);
    };
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[1]/usr/rad*SEUCUSTOM-PCMODE"), "true");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data["selected"] == true);
    CHECK(r.data["value"] == "Text-Based Editor");
}

namespace {

constexpr const wchar_t* kEditor = L"/app/con[0]/ses[0]/wnd[0]/usr/cntlEDITOR/shellcont/shell";

/// The SE38 source editor: SelectAll + ReplaceSelection replace the text, GetLineCount/GetLineText read it back.
struct AbapEditor {
    FakeNode* node;
    std::vector<std::wstring> lines{L"REPORT zold."};
    bool ignore_writes = false;
    void publish() {
        node->method_values[L"GetLineCount"] = _variant_t(static_cast<long>(lines.size()));
        node->method_answers.clear();
        for (size_t i = 0; i < lines.size(); ++i)
            node->method_answers[L"GetLineText(" + std::to_wstring(i + 1) + L")"] = lines[i];
    }
    explicit AbapEditor(FillScene& scene) {
        node = scene.make(L"GuiShell", kEditor);
        node->strings[L"SubType"] = L"AbapEditor";
        node->strings[L"Text"] = L"SAPGUI.AbapEditor.1";   // what the Text property really answers
        node->put_fails = {L"Text"};
        node->methods = {L"SelectAll", L"ReplaceSelection", L"GetLineCount", L"GetLineText"};
        node->on_method = [this](const std::wstring& name, const std::wstring& args) {
            if (name != L"ReplaceSelection" || ignore_writes) return;
            lines.clear();
            std::wstring line;
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == L'\r') continue;
                if (args[i] == L'\n') { lines.push_back(line); line.clear(); continue; }
                line += args[i];
            }
            lines.push_back(line);
            publish();
        };
        publish();
        scene.place(kEditor, node);
    }
};

}  // namespace

TEST_CASE("fill gaps: the ABAP editor's source is replaced and read back", "[fill][issue158]") {
    FillScene scene;
    AbapEditor editor(scene);
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/cntlEDITOR/shellcont/shell"),
                                        "REPORT zffy_fill_test.\nWRITE 'green'.");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    REQUIRE(editor.node->calls.size() >= 2);
    CHECK(editor.node->calls[0].first == L"SelectAll");
    CHECK(editor.node->calls[1] == std::pair<std::wstring, std::wstring>{L"ReplaceSelection",
                                                                          L"REPORT zffy_fill_test.\r\nWRITE 'green'."});
    CHECK(editor.lines == std::vector<std::wstring>{L"REPORT zffy_fill_test.", L"WRITE 'green'."});
    CHECK(r.data["value"] == "REPORT zffy_fill_test.\nWRITE 'green'.");
    CHECK(r.data["source_total_lines"] == 2);
    CHECK(r.data["element_subtype"] == "AbapEditor");
}

TEST_CASE("fill gaps: the ABAP editor in display mode is refused although it accepts text", "[fill][issue158]") {
    FillScene scene;
    AbapEditor editor(scene);
    scene.save->bools[L"Changeable"] = false;   // display mode: Save is off, the editor still says Changeable
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/cntlEDITOR/shellcont/shell"), "REPORT zx.");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "ELEMENT_READ_ONLY");
    CHECK(r.error["message"].get<std::string>().find("display mode") != std::string::npos);
    for (const auto& call : editor.node->calls) CHECK(call.first != L"ReplaceSelection");
    CHECK(editor.lines == std::vector<std::wstring>{L"REPORT zold."});
}

TEST_CASE("fill gaps: ABAP source that does not read back as written is an error", "[fill][issue158]") {
    FillScene scene;
    AbapEditor editor(scene);
    editor.ignore_writes = true;
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/cntlEDITOR/shellcont/shell"), "REPORT zx.");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "FILL_NOT_APPLIED");
}

TEST_CASE("fill gaps: another GuiShell control is refused as unsupported, not as a bare exception", "[fill][issue158]") {
    FillScene scene;
    const std::wstring id = L"/app/con[0]/ses[0]/wnd[0]/usr/cntlHTML/shellcont/shell";
    FakeNode* html = scene.make(L"GuiShell", id);
    html->strings[L"SubType"] = L"HTMLViewer";
    html->put_fails = {L"Text"};
    scene.place(id, html);
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/cntlHTML/shellcont/shell"), "x");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "UNSUPPORTED_CONTROL");
    CHECK(r.error["element_type"] == "GuiShell");
    CHECK(r.error["element_subtype"] == "HTMLViewer");
}
