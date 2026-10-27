// ComAutomationEngine's actions against a scripted SAP GUI (FakeNode): application -> connection -> session ->
// window -> controls (review 2026-10-25: ~40 engine methods had no test).
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cwchar>
#include <memory>
#include <utility>

#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/com/hresult.h"
#include "include/table_data_extractor.h"
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

TEST_CASE("tree item texts: one value per column; a failed read is empty and the next column is still read", "[com][tree]") {
    // review 2026-10-25: the screen reader and the table extractor each read a node's column values in their own loop
    ScopedDispatchCacheReset cache_reset;
    auto* control = new FakeNode();
    control->strings[L"Type"] = L"GuiShell";
    control->methods = {L"GetItemText"};
    control->method_answers = {{L"GetItemText(N1,COL1)", L"a"}, {L"GetItemText(N1,COL2)", L"b"},
                               {L"GetItemText(N1,HierarchyHeader)", L"h"}};
    control->failing_calls = {L"GetItemText(N1,COL3)"};
    ComGuiElement element(IDispatchPtr(control, false));
    using Calls = std::vector<std::pair<std::wstring, std::wstring>>;

    SECTION("skip_hierarchy (the screen reader's flag) skips the hierarchy and unnamed columns") {
        CHECK(as_tree(element).item_texts("N1", {"HierarchyHeader", "COL1", "", "COL3", "COL2"}, true) ==
              std::vector<std::string>{"", "a", "", "", "b"});
        CHECK(control->calls == Calls{{L"GetItemText", L"N1,COL1"}, {L"GetItemText", L"N1,COL3"}, {L"GetItemText", L"N1,COL2"}});
    }
    SECTION("without skip_hierarchy (the table extractor's flag) every column is asked, as before") {
        CHECK(as_tree(element).item_texts("N1", {"HierarchyHeader", "COL3", "COL2"}, false) ==
              std::vector<std::string>{"h", "", "b"});
        CHECK(control->calls == Calls{{L"GetItemText", L"N1,HierarchyHeader"}, {L"GetItemText", L"N1,COL3"},
                                      {L"GetItemText", L"N1,COL2"}});
    }
}

TEST_CASE("tree shell extraction: each node path is read once; nodes nest, data columns and texts come along", "[com][tree]") {
    // Codex on #88: the hierarchy used to ask SAP for every node's path a second time
    ScopedDispatchCacheReset cache_reset;
    auto* tree = new FakeNode();
    tree->strings[L"Type"] = L"GuiShell";
    tree->strings[L"SubType"] = L"Tree";
    auto* keys = new FakeNode();
    auto* order = new FakeNode();
    IDispatchPtr keys_holder(keys, false), order_holder(order, false);
    keys->values = {_variant_t(L"N1"), _variant_t(L"N2")};
    order->values = {_variant_t(L"HierarchyHeader"), _variant_t(L"COL1")};
    tree->methods = {L"GetAllNodeKeys", L"GetNodeTextByKey", L"GetNodePathByKey", L"GetItemText"};
    tree->dispatches[L"GetAllNodeKeys"] = keys;
    tree->dispatches[L"ColumnOrder"] = order;
    tree->method_answers = {{L"GetNodeTextByKey(N1)", L"Folder"}, {L"GetNodeTextByKey(N2)", L"Leaf"},
                            {L"GetNodePathByKey(N1)", L"1"}, {L"GetNodePathByKey(N2)", L"1\\1"},
                            {L"GetItemText(N1,COL1)", L"v1"}, {L"GetItemText(N2,COL1)", L"v2"}};
    const auto element = ComGuiElement::create(IDispatchPtr(tree, false));

    const TreeData data = TableDataExtractor().extract_tree_data(element);
    CHECK(data.columns == std::vector<std::string>{"COL1"});
    REQUIRE(data.nodes.size() == 1);
    CHECK(data.nodes[0].text == "Folder");
    CHECK(data.nodes[0].column_values == std::vector<std::string>{"v1"});
    REQUIRE(data.nodes[0].children.size() == 1);
    CHECK(data.nodes[0].children[0].text == "Leaf");
    CHECK(data.nodes[0].children[0].level == 1);
    CHECK(data.nodes[0].children[0].column_values == std::vector<std::string>{"v2"});
    const auto path_reads = std::count_if(tree->calls.begin(), tree->calls.end(),
                                          [](const auto& call) { return call.first == L"GetNodePathByKey"; });
    CHECK(path_reads == 2);   // once per node
}

TEST_CASE("hresult_hex prints an HRESULT unsigned, as Windows documents it", "[com][hresult]") {
    // the {:08X} of a signed HRESULT printed 0x-7FFDFFF7 in ~35 error and log messages
    CHECK(fairyfly::hresult_hex(DISP_E_EXCEPTION) == "0x80020009");
    CHECK(fairyfly::hresult_hex(E_FAIL) == "0x80004005");
    CHECK(fairyfly::hresult_hex(S_OK) == "0x00000000");
}
