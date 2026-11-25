// Issue #158: menu paths as SAP GUI themes show them ("Settings...") and a MENU_ITEM_NOT_FOUND that says where the path
// left the menu, what is there instead and on which screen (Utilities > Settings exists only inside the ABAP editor).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/com/wrapper.h"
#include "include/com_automation_engine.h"
#include "include/menu_navigation.h"
#include "include/read_only_guard.h"
#include "support/fake_dispatch.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using namespace fairyfly::test;
using json = nlohmann::json;

TEST_CASE("menu gaps: a trailing ellipsis and extra blanks do not stop a match", "[menu][issue158]") {
    CHECK(menu_label_matches("Settings", "Settings..."));
    CHECK(menu_label_matches("Settings...", "Settings"));
    CHECK(menu_label_matches("Settings", "Settings\xE2\x80\xA6"));   // U+2026
    CHECK(menu_label_matches("Settings", "Settings ..."));
    CHECK(menu_label_matches("Display  Object  List", "display object list"));
    CHECK_FALSE(menu_label_matches("Settings", "Sett"));
    CHECK_FALSE(menu_label_matches("Settings", "..."));
    // the read-only guard judges the same label
    CHECK(normalize_menu_segment("Sa&ve...") == "save");
    CHECK(normalize_menu_segment("Settings\xE2\x80\xA6") == "settings");
}

TEST_CASE("menu gaps: the closest labels of a level, nearest first", "[menu][issue158]") {
    const std::vector<std::string> labels{"Settings", "Versions", "Split Screen Editor", "More Utilities"};
    const auto closest = closest_menu_labels("Setings", labels);
    REQUIRE_FALSE(closest.empty());
    CHECK(closest.front() == "Settings");
    CHECK(closest_menu_labels("Zzzzzzzz", labels).empty());
    CHECK(closest_menu_labels("utilities", labels) == std::vector<std::string>{"More Utilities"});
}

namespace {

constexpr const wchar_t* kSession = L"/app/con[0]/ses[0]";

struct MenuScene {
    ScopedDispatchCacheReset cache_reset;
    std::vector<FakeNode*> nodes;
    FakeNode *app, *connections, *connection, *sessions, *session, *window, *menubar, *settings;

    FakeNode* make(const wchar_t* type, const std::wstring& id, const wchar_t* text = L"") {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        node->strings[L"Text"] = text;
        node->bools[L"Changeable"] = true;
        node->bools[L"Enabled"] = true;
        nodes.push_back(node);
        return node;
    }
    FakeNode* children(std::vector<IDispatch*> items) {
        auto* collection = make(L"GuiComponentCollection", L"");
        collection->items = std::move(items);
        return collection;
    }
    MenuScene() {
        app = make(L"GuiApplication", L"/app");
        connections = make(L"GuiComponentCollection", L"");
        connection = make(L"GuiConnection", L"/app/con[0]");
        sessions = make(L"GuiComponentCollection", L"");
        session = make(L"GuiSession", kSession);
        window = make(L"GuiMainWindow", L"/app/con[0]/ses[0]/wnd[0]", L"ABAP Editor: Change Report ZFFY_FILL_TEST");
        menubar = make(L"GuiMenubar", L"/app/con[0]/ses[0]/wnd[0]/mbar");
        auto* program = make(L"GuiMenu", L"/app/con[0]/ses[0]/wnd[0]/mbar/menu[0]", L"&Program");
        auto* utilities = make(L"GuiMenu", L"/app/con[0]/ses[0]/wnd[0]/mbar/menu[3]", L"&Utilities");
        settings = make(L"GuiMenu", L"/app/con[0]/ses[0]/wnd[0]/mbar/menu[3]/menu[0]", L"Se&ttings");
        settings->methods = {L"Select"};
        auto* versions = make(L"GuiMenu", L"/app/con[0]/ses[0]/wnd[0]/mbar/menu[3]/menu[1]", L"&Versions");
        utilities->dispatches[L"Children"] = children({settings, versions});
        program->dispatches[L"Children"] = children({});
        menubar->dispatches[L"Children"] = children({program, utilities});
        app->dispatches[L"Connections"] = connections;
        connections->items = {connection};
        connection->dispatches[L"Children"] = sessions;
        connection->bools[L"DisabledByServer"] = false;
        sessions->items = {session};
        session->dispatches[L"ActiveWindow"] = window;
        session->bools[L"Busy"] = false;
        auto* info = make(L"GuiSessionInfo", L"");
        info->strings[L"Transaction"] = L"SE38";
        info->strings[L"Program"] = L"SAPLWBABAP";
        info->ints[L"ScreenNumber"] = 100;
        session->dispatches[L"Info"] = info;
        for (auto [full, node] : {std::pair<std::wstring, FakeNode*>{L"/app/con[0]/ses[0]/wnd[0]", window},
                                  std::pair<std::wstring, FakeNode*>{L"/app/con[0]/ses[0]/wnd[0]/mbar", menubar}}) {
            session->find_by_id[full] = node;
            session->find_by_id[full.substr(std::wcslen(kSession) + 1)] = node;
        }
    }
    ~MenuScene() { for (auto* node : nodes) node->Release(); }
    std::unique_ptr<ComAutomationEngine> engine() {
        app->AddRef();
        return std::make_unique<ComAutomationEngine>(std::make_shared<ComGuiApplication>(IDispatchPtr(app, false)));
    }
};

}  // namespace

TEST_CASE("menu gaps: Utilities/Settings... selects Settings", "[menu][issue158]") {
    MenuScene scene;
    auto engine = scene.engine();
    const Result r = engine->select_menu("Utilities/Settings...", "", false);
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    CHECK(scene.settings->select_calls == 1);
}

TEST_CASE("menu gaps: an unknown entry names the segment, what is there and the screen", "[menu][issue158]") {
    MenuScene scene;
    auto engine = scene.engine();
    const Result r = engine->select_menu("Utilities/Setings", "", false);
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "MENU_ITEM_NOT_FOUND");
    CHECK(r.error["failed_segment"] == "Setings");
    CHECK(r.error["matched_path"] == "Utilities");
    CHECK(r.error["available"] == json::array({"Settings", "Versions"}));
    CHECK(r.error["closest"] == json::array({"Settings"}));
    CHECK(r.error["transaction"] == "SE38");
    CHECK(r.error["program"] == "SAPLWBABAP");
    CHECK(r.error["message"].get<std::string>().find("Did you mean 'Settings'") != std::string::npos);
}

TEST_CASE("menu gaps: an unknown top-level menu lists the menu bar", "[menu][issue158]") {
    MenuScene scene;
    auto engine = scene.engine();
    const Result r = engine->select_menu("Goto/Attributes", "", false);
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["failed_segment"] == "Goto");
    CHECK(r.error["matched_path"] == "");
    CHECK(r.error["available"] == json::array({"Program", "Utilities"}));
}
