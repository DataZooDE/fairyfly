// ComAutomationEngine's actions against a scripted SAP GUI (FakeNode): application -> connection -> session ->
// window -> controls (review 2026-10-25: ~40 engine methods had no test).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <utility>

#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/com/control_views.h"
#include "include/com/tree_view.h"
#include "include/com_automation_engine.h"
#include "support/fake_dispatch.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using namespace fairyfly::test;
using json = nlohmann::json;

namespace {

constexpr const wchar_t* kSession = L"/app/con[0]/ses[0]";
constexpr const wchar_t* kWnd0 = L"/app/con[0]/ses[0]/wnd[0]";
constexpr const wchar_t* kField = L"/app/con[0]/ses[0]/wnd[0]/usr/txtRSYST-BNAME";
constexpr const wchar_t* kButton = L"/app/con[0]/ses[0]/wnd[0]/tbar[1]/btn[8]";

/// application -> one connection -> one session with wnd[0], a text field and a button.
struct SapScene {
    ScopedDispatchCacheReset cache_reset;
    std::vector<FakeNode*> nodes;
    FakeNode *app, *connections, *connection, *sessions, *session, *window, *field, *button;

    FakeNode* make(const wchar_t* type, const wchar_t* id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        nodes.push_back(node);
        return node;
    }
    SapScene() {
        app = make(L"GuiApplication", L"/app");
        connections = make(L"GuiComponentCollection", L"");
        connection = make(L"GuiConnection", L"/app/con[0]");
        sessions = make(L"GuiComponentCollection", L"");
        session = make(L"GuiSession", kSession);
        window = make(L"GuiMainWindow", kWnd0);
        field = make(L"GuiTextField", kField);
        button = make(L"GuiButton", kButton);
        app->dispatches[L"Connections"] = connections;
        connections->items = {connection};
        connection->dispatches[L"Children"] = sessions;
        connection->bools[L"DisabledByServer"] = false;
        sessions->items = {session};
        session->dispatches[L"ActiveWindow"] = window;
        // FindById takes the full id and the relative one the commands pass on (as SAP GUI does)
        for (auto [full, node] : {std::pair{kWnd0, window}, std::pair{kField, field}, std::pair{kButton, button}}) {
            session->find_by_id[full] = node;
            session->find_by_id[std::wstring(full).substr(std::wcslen(kSession) + 1)] = node;
        }
        field->bools[L"Changeable"] = true;
        field->strings[L"Text"] = L"";
        button->bools[L"Changeable"] = true;
        button->methods = {L"Press"};
    }
    ~SapScene() {
        for (auto* node : nodes) node->Release();
    }
    std::unique_ptr<ComAutomationEngine> engine() {
        app->AddRef();   // the application wrapper owns one reference
        return std::make_unique<ComAutomationEngine>(std::make_shared<ComGuiApplication>(IDispatchPtr(app, false), 0));
    }
};

}  // namespace

TEST_CASE("engine: fill writes the field's Text on the bound session", "[engine][actions]") {
    SapScene scene;
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/txtRSYST-BNAME"), "DEVELOPER");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    CHECK(scene.field->strings[L"Text"] == L"DEVELOPER");
}

TEST_CASE("engine: click presses the button", "[engine][actions]") {
    SapScene scene;
    auto engine = scene.engine();
    const Result r = engine->click_element(ElementId("wnd[0]/tbar[1]/btn[8]"));
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    REQUIRE(scene.button->calls.size() == 1);
    CHECK(scene.button->calls[0].first == L"Press");
}

TEST_CASE("engine: a missing element in the main window while a popup is open says how to retry there", "[engine][actions]") {
    SapScene scene;
    FakeNode* popup = scene.make(L"GuiModalWindow", L"/app/con[0]/ses[0]/wnd[1]");
    scene.session->dispatches[L"ActiveWindow"] = popup;
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/txtMISSING"), "x");
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "ELEMENT_NOT_FOUND");
    CHECK(r.error.value("window_mismatch", false));
    CHECK(r.error["active_window"] == "/app/con[0]/ses[0]/wnd[1]");
    CHECK(r.error["suggestions"].dump().find("fairyfly element fill /app/con[0]/ses[0]/wnd[1]/usr/txtMISSING") != std::string::npos);
}

TEST_CASE("engine: a field SAP shows as not changeable is refused and not written", "[engine][actions]") {
    SapScene scene;
    scene.field->bools[L"Changeable"] = false;
    auto engine = scene.engine();
    const Result r = engine->fill_field(ElementId("wnd[0]/usr/txtRSYST-BNAME"), "DEVELOPER");
    INFO(r.to_json().dump());
    CHECK(r.status == Result::Status::Error);
    CHECK(scene.field->strings[L"Text"].empty());
    for (const auto& put : scene.field->puts) CHECK(put.first != L"Text");
}

TEST_CASE("engine: a transaction is started on the bound session", "[engine][actions]") {
    SapScene scene;
    auto engine = scene.engine();
    FakeNode* info = scene.make(L"GuiSessionInfo", L"");
    info->strings[L"Transaction"] = L"SU01";   // where the session is afterwards
    scene.session->dispatches[L"Info"] = info;
    const Result r = engine->execute_transaction("SU01");
    INFO(r.to_json().dump());
    CHECK(scene.session->start_transaction_calls == 1);
    CHECK(scene.session->start_transaction_code == L"SU01");
    CHECK(r.status == Result::Status::Success);
}

TEST_CASE("engine: reading a field answers its text", "[engine][actions]") {
    SapScene scene;
    scene.field->strings[L"Text"] = L"DEVELOPER";
    auto engine = scene.engine();
    const Result r = engine->read_field(ElementId("wnd[0]/usr/txtRSYST-BNAME"));
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    CHECK(r.data.dump().find("DEVELOPER") != std::string::npos);
}

TEST_CASE("engine: a launch through SAP GUI's OpenConnection binds the new session", "[engine][actions][launch]") {
    SapScene scene;
    auto engine = scene.engine();
    FakeNode* opened = scene.make(L"GuiConnection", L"/app/con[1]");
    FakeNode* opened_sessions = scene.make(L"GuiComponentCollection", L"");
    FakeNode* new_session = scene.make(L"GuiSession", L"/app/con[1]/ses[0]");
    opened->dispatches[L"Children"] = opened_sessions;
    opened->bools[L"DisabledByServer"] = false;
    opened_sessions->items = {new_session};
    scene.app->methods.insert(L"OpenConnection");
    scene.app->dispatches[L"OpenConnection"] = opened;
    const Result r = engine->launch_connection("BIG", false);
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Success);
    REQUIRE(scene.app->calls.size() == 1);
    CHECK(scene.app->calls[0].first == L"OpenConnection");
    CHECK(scene.app->calls[0].second.rfind(L"BIG,", 0) == 0);   // the SAP Logon entry first
    CHECK(r.data.dump().find("/app/con[1]/ses[0]") != std::string::npos);
}

TEST_CASE("engine: a launch SAP GUI cannot open, without the sapshcut fallback, names the entry", "[engine][actions][launch]") {
    SapScene scene;
    auto engine = scene.engine();   // the fake application has no OpenConnection
    const Result r = engine->launch_connection("BIG", false);
    INFO(r.to_json().dump());
    REQUIRE(r.status == Result::Status::Error);
    CHECK(r.error["code"] == "CONNECTION_OPEN_FAILED");
    CHECK(r.error["connection_name"] == "BIG");
}

TEST_CASE("collection_member_items walks Count/Item of a member collection, up to a limit", "[com][collection]") {
    // review 2026-10-25: GetAllNodeKeys, GetColumnNames and ColumnOrder each repeated this walk
    ScopedDispatchCacheReset cache_reset;
    auto* owner = new FakeNode();
    IDispatchPtr owner_holder(owner, false);
    auto* collection = new FakeNode();
    IDispatchPtr collection_holder(collection, false);
    std::vector<IDispatchPtr> children;
    for (int i = 0; i < 3; ++i) {
        children.emplace_back(new FakeNode(), false);
        collection->items.push_back(children.back());
    }
    owner->methods.insert(L"GetAllNodeKeys");
    owner->dispatches[L"GetAllNodeKeys"] = collection;
    owner->dispatches[L"ColumnOrder"] = collection;

    CHECK(fairyfly::sap::collection_member_items(owner, L"GetAllNodeKeys", DISPATCH_METHOD).size() == 3);
    CHECK(fairyfly::sap::collection_member_items(owner, L"ColumnOrder", DISPATCH_PROPERTYGET, 2).size() == 2);
    collection->count_override = 5;   // Item(3), Item(4) fail: skipped
    CHECK(fairyfly::sap::collection_member_items(owner, L"ColumnOrder", DISPATCH_PROPERTYGET).size() == 3);
    CHECK(fairyfly::sap::collection_member_items(owner, L"Missing", DISPATCH_PROPERTYGET).empty());   // not a collection
}

TEST_CASE("collection walks: each caller keeps its own items, every index is VT_I4, a VT_I2 Count reads as none", "[com][collection]") {
    // Codex on #84: GetColumnNames used to pass _variant_t(int) (VT_INT); the shared walk passes VT_I4 like the others
    ScopedDispatchCacheReset cache_reset;
    auto* control = new FakeNode();
    control->strings[L"Type"] = L"GuiShell";
    auto* keys = new FakeNode();
    auto* names = new FakeNode();
    auto* order = new FakeNode();
    IDispatchPtr keys_holder(keys, false), names_holder(names, false), order_holder(order, false);
    control->methods = {L"GetAllNodeKeys", L"GetColumnNames"};
    control->dispatches[L"GetAllNodeKeys"] = keys;
    control->dispatches[L"GetColumnNames"] = names;
    control->dispatches[L"ColumnOrder"] = order;
    keys->values = {_variant_t(L"N1"), _variant_t(L""), _variant_t(7L), _variant_t(true)};
    names->values = {_variant_t(L"COL1"), _variant_t(L""), _variant_t(3L)};
    order->values = {_variant_t(L"C1"), _variant_t(L"")};
    ComGuiElement element(IDispatchPtr(control, false));

    CHECK(as_tree(element).get_all_node_keys() == std::vector<std::string>{"N1", "7"});        // non-empty BSTR or VT_I4
    CHECK(as_tree(element).get_tree_column_names() == std::vector<std::string>{"COL1", ""});   // every BSTR
    CHECK(as_grid(element).get_column_order() == std::vector<std::string>{"C1"});              // non-empty strings
    for (auto* collection : {keys, names, order}) {
        CHECK(collection->item_index_types.size() == collection->values.size());
        CHECK(std::all_of(collection->item_index_types.begin(), collection->item_index_types.end(),
                          [](VARTYPE vt) { return vt == VT_I4; }));
    }
    order->count_type = VT_I2;   // only a VT_I4 Count is read, as before
    CHECK(as_grid(element).get_column_order().empty());
}
