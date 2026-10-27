// The audit trail's COM screen source against a scripted SAP GUI (review 2026-10-25 test gap 6): GetObjectTree
// failing falls back to the element walk, a structure too deep is incomplete, a password's text is never read.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include "include/com/wrapper.h"
#include "include/link/trail_recorder.h"
#include "include/link/trail_screen_source.h"
#include "support/fake_dispatch.h"

using namespace fairyfly;
using namespace fairyfly::test;
namespace trail = fairyfly::link::trail;

namespace {

constexpr const wchar_t* kWnd = L"/app/con[0]/ses[0]/wnd[0]";

/// session -> wnd[0] -> usr -> (a text field, a password field); GetObjectTree fails.
struct TrailScene {
    ScopedDispatchCacheReset cache_reset;
    std::vector<FakeNode*> nodes;
    FakeNode *session, *window, *window_children, *usr, *usr_children, *user, *password;

    FakeNode* make(const wchar_t* type, const std::wstring& id) {
        auto* node = new FakeNode();
        node->strings[L"Type"] = type;
        node->strings[L"Id"] = id;
        nodes.push_back(node);
        return node;
    }
    static void children(FakeNode* parent, FakeNode* collection, std::vector<IDispatch*> items) {
        collection->items = std::move(items);
        parent->dispatches[L"Children"] = collection;
    }
    TrailScene() {
        session = make(L"GuiSession", L"/app/con[0]/ses[0]");
        window = make(L"GuiMainWindow", kWnd);
        window_children = make(L"GuiComponentCollection", L"");
        usr = make(L"GuiUserArea", std::wstring(kWnd) + L"/usr");
        usr_children = make(L"GuiComponentCollection", L"");
        user = make(L"GuiTextField", std::wstring(kWnd) + L"/usr/txtRSYST-BNAME");
        password = make(L"GuiPasswordField", std::wstring(kWnd) + L"/usr/pwdRSYST-BCODE");
        session->dispatches[L"ActiveWindow"] = window;
        session->object_tree_hresult = E_FAIL;   // no bulk read: the element walk has to do it
        children(window, window_children, {usr});
        children(usr, usr_children, {user, password});
        user->strings[L"Text"] = L"DEVELOPER";
        password->strings[L"Text"] = L"secret";
        for (auto* n : {user, password}) {   // the full id and the relative one value() is given
            session->find_by_id[n->strings[L"Id"]] = n;
            session->find_by_id[n->strings[L"Id"].substr(std::wstring(L"/app/con[0]/ses[0]/").size())] = n;
        }
    }
    ~TrailScene() {
        for (auto* node : nodes) node->Release();
    }
    std::shared_ptr<trail::ScreenSource> source() {
        session->AddRef();
        auto wrapped = sap::ComGuiSession::create(IDispatchPtr(session, false));
        return trail::make_com_screen_source_for([wrapped] { return wrapped; });
    }
};

const trail::Field* field(const trail::ScreenState& s, const std::string& id) {
    for (const auto& f : s.fields)
        if (f.id == id) return &f;
    return nullptr;
}

}  // namespace

TEST_CASE("trail source: without GetObjectTree the element walk reads the screen, never a password's text", "[trail][source]") {
    TrailScene scene;
    const trail::ScreenState s = scene.source()->state({});
    CHECK(s.complete);
    CHECK(s.window_id == "wnd[0]");
    const trail::Field* user = field(s, "wnd[0]/usr/txtRSYST-BNAME");
    const trail::Field* password = field(s, "wnd[0]/usr/pwdRSYST-BCODE");
    REQUIRE(user);
    REQUIRE(password);
    CHECK(user->value == "DEVELOPER");
    CHECK(password->value.empty());
    CHECK(scene.password->reads[L"Text"] == 0);   // not read at all, not just dropped
}

TEST_CASE("trail source: a structure deeper than the walk's bound leaves the screen incomplete", "[trail][source]") {
    TrailScene scene;
    // 45 nested containers below the user area: deeper than the walk goes (40)
    FakeNode* parent = scene.usr;
    for (int depth = 0; depth < 45; ++depth) {
        FakeNode* child = scene.make(L"GuiSimpleContainer", parent->strings[L"Id"] + L"/sub" + std::to_wstring(depth));
        FakeNode* collection = scene.make(L"GuiComponentCollection", L"");
        TrailScene::children(parent, collection, {child});
        parent = child;
    }
    const trail::ScreenState s = scene.source()->state({});
    CHECK_FALSE(s.complete);
    CHECK(s.fields.empty());   // nothing half-read is used
}

TEST_CASE("trail source: the value of a password field is empty and its text is not read", "[trail][source]") {
    TrailScene scene;
    auto source = scene.source();
    CHECK(source->value("wnd[0]/usr/txtRSYST-BNAME", std::nullopt, "") == std::optional<std::string>("DEVELOPER"));
    CHECK(source->value("wnd[0]/usr/pwdRSYST-BCODE", std::nullopt, "") == std::optional<std::string>(""));
    CHECK(scene.password->reads[L"Text"] == 0);
}
