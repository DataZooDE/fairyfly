// Issue #158: a screen read with tabs on a popup with a nested tabstrip (SE38 > Utilities > Settings: G_TABSTRIP, and
// under its "ABAP Editor" page the strip G_ED_TABSTR) failed with TAB_RESTORE_FAILED and no data, and left the popup on
// the last outer tab. Selecting another outer page removes the inner strip (its ids no longer resolve).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cwchar>
#include <stdexcept>
#include <string>
#include <vector>

#include "include/com/wrapper.h"
#include "include/screen_reader.h"
#include "support/fake_dispatch.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using namespace fairyfly::test;
using json = nlohmann::json;

namespace {

struct NestedTabScene {
    static constexpr const wchar_t* kWnd = L"/app/con[0]/ses[0]/wnd[1]";
    static constexpr const wchar_t* kUsr = L"/app/con[0]/ses[0]/wnd[1]/usr";
    static constexpr const wchar_t* kOuter = L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP";
    static constexpr const wchar_t* kGeneral = L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0600";
    static constexpr const wchar_t* kEditor = L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0100";
    static constexpr const wchar_t* kInner =
        L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100/tabsG_ED_TABSTR";
    static constexpr const wchar_t* kEdit =
        L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100/tabsG_ED_TABSTR/tabpEDIT";
    static constexpr const wchar_t* kPretty =
        L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100/tabsG_ED_TABSTR/tabpPRETTY";
    static constexpr const wchar_t* kRadio =
        L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100/tabsG_ED_TABSTR/tabpEDIT/radSEUCUSTOM-PCMODE";
    static constexpr const wchar_t* kCheck =
        L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0600/chkSEUCUSTOM-FLAG";

    ScopedDispatchCacheReset cache_reset;
    std::vector<FakeNode*> nodes;
    FakeNode *session, *window, *outer, *general, *editor, *inner, *edit, *pretty;
    std::vector<std::wstring> selections;   ///< every Select in order (tab id)
    bool editor_page_gone = false;          ///< the ABAP Editor tab cannot be selected again (a real restore failure)

    FakeNode* make(const wchar_t* type, const wchar_t* id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        nodes.push_back(node);
        return node;
    }
    FakeNode* children(std::vector<IDispatch*> items) {
        auto* collection = make(L"GuiCollection", L"");
        collection->items = std::move(items);
        return collection;
    }
    void show_inner(bool visible) {
        for (const wchar_t* id : {kInner, kEdit, kPretty, kRadio}) session->find_by_id.erase(id);
        if (!visible) return;
        session->find_by_id[kInner] = inner;
        session->find_by_id[kEdit] = edit;
        session->find_by_id[kPretty] = pretty;
        session->find_by_id[kRadio] = nodes_by_id(kRadio);
    }
    FakeNode* nodes_by_id(const wchar_t* id) {
        for (auto* node : nodes)
            if (node->strings[L"Id"] == id) return node;
        return nullptr;
    }
    NestedTabScene() {
        session = make(L"GuiSession", L"/app/con[0]/ses[0]");
        auto* info = make(L"GuiSessionInfo", L"");
        info->strings[L"Transaction"] = L"SE38";
        window = make(L"GuiModalWindow", kWnd);
        window->strings[L"Text"] = L"User-Specific Settings";
        auto* usr = make(L"GuiUserArea", kUsr);
        outer = make(L"GuiTabStrip", kOuter);
        general = make(L"GuiTab", kGeneral);
        general->strings[L"Text"] = L"Workbench (General)";
        auto* check = make(L"GuiCheckBox", kCheck);
        check->strings[L"Text"] = L"Flag";
        check->bools[L"Selected"] = false;
        check->bools[L"Changeable"] = true;
        general->dispatches[L"Children"] = children({check});
        editor = make(L"GuiTab", kEditor);
        editor->strings[L"Text"] = L"ABAP Editor";
        auto* toolarea = make(L"GuiSimpleContainer",
                              L"/app/con[0]/ses[0]/wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100");
        inner = make(L"GuiTabStrip", kInner);
        edit = make(L"GuiTab", kEdit);
        edit->strings[L"Text"] = L"Editor";
        auto* radio = make(L"GuiRadioButton", kRadio);
        radio->strings[L"Text"] = L"Text-Based Editor";
        radio->bools[L"Selected"] = false;
        radio->bools[L"Changeable"] = true;
        edit->dispatches[L"Children"] = children({radio});
        pretty = make(L"GuiTab", kPretty);
        pretty->strings[L"Text"] = L"Pretty Printer";
        pretty->dispatches[L"Children"] = children({});
        inner->dispatches[L"Children"] = children({edit, pretty});
        inner->dispatches[L"SelectedTab"] = edit;
        toolarea->dispatches[L"Children"] = children({inner});
        editor->dispatches[L"Children"] = children({toolarea});
        outer->dispatches[L"Children"] = children({editor, general});   // live: the original page is not the last one
        outer->dispatches[L"SelectedTab"] = editor;   // the popup opens on "ABAP Editor" / "Editor"
        usr->dispatches[L"Children"] = children({outer});
        window->dispatches[L"Children"] = children({usr});
        session->dispatches[L"ActiveWindow"] = window;
        session->dispatches[L"Info"] = info;
        session->bools[L"Busy"] = false;
        auto& ids = session->find_by_id;
        ids[kWnd] = window;
        ids[L"wnd[1]"] = window;
        ids[kUsr] = usr;
        ids[kOuter] = outer;
        ids[kGeneral] = general;
        ids[kEditor] = editor;
        ids[kCheck] = check;
        ids[toolarea->strings[L"Id"]] = toolarea;
        show_inner(true);
        general->on_select = [this] {
            selections.push_back(kGeneral);
            outer->dispatches[L"SelectedTab"] = general;
            show_inner(false);   // another outer page: the inner strip is gone
            session->find_by_id[kCheck] = nodes_by_id(kCheck);
        };
        editor->on_select = [this] {
            selections.push_back(kEditor);
            if (editor_page_gone) throw std::runtime_error("tab cannot be selected");
            outer->dispatches[L"SelectedTab"] = editor;
            show_inner(true);   // the inner strip comes back on the page it showed last
        };
        edit->on_select = [this] {
            selections.push_back(kEdit);
            inner->dispatches[L"SelectedTab"] = edit;
        };
        pretty->on_select = [this] {
            selections.push_back(kPretty);
            inner->dispatches[L"SelectedTab"] = pretty;
        };
    }
    ~NestedTabScene() { for (auto* node : nodes) node->Release(); }
    ComGuiSessionPtr session_wrapper() { return ComGuiSession::create(IDispatchPtr(session)); }
    std::wstring selected(FakeNode* strip) {
        return static_cast<FakeNode*>(strip->dispatches[L"SelectedTab"])->strings[L"Id"];
    }
};

std::vector<std::string> tab_ids_read(const json& data) {
    std::vector<std::string> ids;
    for (const auto& tab : data.value("tabs_content", json::array())) ids.push_back(tab.value("tab_id", ""));
    return ids;
}

}  // namespace

TEST_CASE("nested tabs: every tab is read and both strips end where they started", "[screen][tabs][issue158]") {
    NestedTabScene scene;
    ScreenReader reader(scene.session_wrapper());
    const Result result = reader.read_with_tabs();
    INFO(result.to_json().dump());
    REQUIRE(result.status == Result::Status::Success);
    CHECK(scene.selected(scene.outer) == NestedTabScene::kEditor);
    CHECK(scene.selected(scene.inner) == NestedTabScene::kEdit);
    const auto read = tab_ids_read(result.data);
    for (const wchar_t* tab : {NestedTabScene::kGeneral, NestedTabScene::kEditor, NestedTabScene::kEdit,
                               NestedTabScene::kPretty}) {
        const std::string id(tab, tab + std::wcslen(tab));
        CHECK(std::find(read.begin(), read.end(), id) != read.end());
    }
    CHECK_FALSE(result.data.contains("tabs_failed"));
    CHECK_FALSE(result.data.contains("tab_restore_failed"));
}

TEST_CASE("nested tabs: a restore that really fails still returns what was read, with a warning", "[screen][tabs][issue158]") {
    NestedTabScene scene;
    ScreenReader reader(scene.session_wrapper());
    scene.general->on_select = [&scene, old = scene.general->on_select] {
        old();
        scene.editor_page_gone = true;   // from now on "ABAP Editor" cannot be selected again
    };
    const Result result = reader.read_with_tabs();
    INFO(result.to_json().dump());
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(result.data.contains("tab_restore_failed"));
    CHECK(result.data["tab_restore_failed"].dump().find("tabp0100") != std::string::npos);
    REQUIRE(result.data.contains("warnings"));
    CHECK(result.data["warnings"].dump().find("could not be restored") != std::string::npos);
    CHECK_FALSE(tab_ids_read(result.data).empty());
    CHECK(result.data.contains("elements"));
}

TEST_CASE("nested tabs: a single tab read on the nested page restores and keeps its data", "[screen][tabs][issue158]") {
    NestedTabScene scene;
    ScreenReader reader(scene.session_wrapper());
    const std::wstring wide(NestedTabScene::kPretty);
    const Result result = reader.read_with_tabs(false, 20, std::string(wide.begin(), wide.end()));
    INFO(result.to_json().dump());
    REQUIRE(result.status == Result::Status::Success);
    CHECK(scene.selected(scene.outer) == NestedTabScene::kEditor);
    CHECK(scene.selected(scene.inner) == NestedTabScene::kEdit);
}

TEST_CASE("screen find: text_contains finds controls by their visible text or tooltip", "[screen][find][issue158]") {
    NestedTabScene scene;
    ScreenReader reader(scene.session_wrapper());
    ScreenFindOptions query;
    query.text_contains = "TEXT-BASED";   // the radio button's caption, any case
    query.limit = 10;
    const Result found = reader.find(query);
    INFO(found.to_json().dump());
    REQUIRE(found.status == Result::Status::Success);
    const auto& elements = found.data.at("elements");
    REQUIRE(elements.size() == 1);
    const std::wstring radio(NestedTabScene::kRadio);
    CHECK(elements[0].value("id", "") == std::string(radio.begin(), radio.end()));
    CHECK(elements[0].value("text", "") == "Text-Based Editor");

    query.text_contains = "no such caption";
    const Result none = reader.find(query);
    REQUIRE(none.status == Result::Status::Success);
    CHECK(none.data.at("elements").empty());
}
