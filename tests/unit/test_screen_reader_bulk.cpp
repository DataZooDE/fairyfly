// ScreenReader bulk (GuiSession.GetObjectTree) mode: parity with the legacy reader on recorded
// screens, the fallback matrix, redaction parity and the targeted per-element reads.
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdlib.h>

#include "include/bulk_screen_reader.h"
#include "include/com/wrapper.h"
#include "include/object_tree.h"
#include "include/screen_element_collector.h"
#include "include/screen_reader.h"
#include "include/sap_gui_base.h"
#include "include/sensitive_data.h"

using namespace fairyfly;
using namespace fairyfly::sap;
using nlohmann::json;

namespace {

// ---------------------------------------------------------------------------------------------
// Recorded screens
// ---------------------------------------------------------------------------------------------

std::string read_fixture(const std::string& name) {
    std::ifstream in(std::string(FAIRYFLY_UNIT_FIXTURES) + "/object_tree/" + name, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

const char* kScreens[] = {"SM50", "SM21_sel", "SU01_display", "ST22_list",
                          "SE16_TADIR", "RZ11_detail", "SE80"};

// ---------------------------------------------------------------------------------------------
// A small scriptable SAP object graph: properties by name, FindById, Children (Count/Item),
// GuiSession.GetObjectTree. DISPIDs come from one process-wide name table.
// ---------------------------------------------------------------------------------------------

class Node final : public IDispatch {
public:
    std::map<std::wstring, std::wstring> strings;
    std::map<std::wstring, bool> bools;
    std::map<std::wstring, IDispatch*> dispatches;
    std::map<std::wstring, IDispatch*> find_by_id;
    std::vector<IDispatch*> items;
    std::set<std::wstring> unknown_names;
    std::map<std::wstring, int> reads;
    int object_tree_calls = 0;
    int select_calls = 0;
    std::function<void()> on_select;
    std::wstring object_tree_payload;
    HRESULT object_tree_hresult = S_OK;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid == IID_IUnknown || iid == IID_IDispatch) {
            *object = static_cast<IDispatch*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG remaining = --references_;
        if (!remaining) delete this;
        return remaining;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* count) override {
        if (!count) return E_POINTER;
        *count = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT count, LCID,
                                            DISPID* ids) override {
        if (!names || !ids || count != 1) return E_INVALIDARG;
        if (unknown_names.count(names[0])) return DISP_E_UNKNOWNNAME;
        auto& table = name_table();
        for (size_t i = 0; i < table.size(); ++i) {
            if (table[i] == names[0]) { *ids = static_cast<DISPID>(7000 + i); return S_OK; }
        }
        table.emplace_back(names[0]);
        *ids = static_cast<DISPID>(7000 + table.size() - 1);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags, DISPPARAMS* params,
                                     VARIANT* result, EXCEPINFO*, UINT*) override {
        const auto& table = name_table();
        if (id < 7000 || static_cast<size_t>(id - 7000) >= table.size()) return DISP_E_MEMBERNOTFOUND;
        const std::wstring& name = table[id - 7000];
        if ((flags & DISPATCH_METHOD) && name == L"Select") {
            ++select_calls;
            if (on_select) on_select();
            return S_OK;
        }
        if ((flags & DISPATCH_METHOD) && name == L"GetObjectTree") {
            ++object_tree_calls;
            if (FAILED(object_tree_hresult)) return object_tree_hresult;
            if (!result) return S_OK;
            VariantInit(result);
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(object_tree_payload.c_str());
            return S_OK;
        }
        if ((flags & DISPATCH_METHOD) && name == L"FindById") {
            if (!result || !params || params->cArgs != 1 || params->rgvarg[0].vt != VT_BSTR)
                return DISP_E_BADPARAMCOUNT;
            auto it = find_by_id.find(params->rgvarg[0].bstrVal);
            if (it == find_by_id.end()) return DISP_E_EXCEPTION;
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = it->second;
            it->second->AddRef();
            return S_OK;
        }
        if (!result) return DISP_E_MEMBERNOTFOUND;
        if (name == L"Item" && (flags & (DISPATCH_METHOD | DISPATCH_PROPERTYGET))) {
            if (!params || params->cArgs != 1 || params->rgvarg[0].lVal < 0 ||
                static_cast<size_t>(params->rgvarg[0].lVal) >= items.size())
                return DISP_E_BADINDEX;
            VariantInit(result);
            result->vt = VT_DISPATCH;
            result->pdispVal = items[params->rgvarg[0].lVal];
            result->pdispVal->AddRef();
            return S_OK;
        }
        if (!(flags & DISPATCH_PROPERTYGET)) return DISP_E_MEMBERNOTFOUND;
        ++reads[name];
        VariantInit(result);
        if (name == L"Count") {
            result->vt = VT_I4;
            result->lVal = static_cast<long>(items.size());
            return S_OK;
        }
        if (auto d = dispatches.find(name); d != dispatches.end()) {
            result->vt = VT_DISPATCH;
            result->pdispVal = d->second;
            d->second->AddRef();
            return S_OK;
        }
        if (auto b = bools.find(name); b != bools.end()) {
            result->vt = VT_BOOL;
            result->boolVal = b->second ? VARIANT_TRUE : VARIANT_FALSE;
            return S_OK;
        }
        if (auto v = strings.find(name); v != strings.end()) {
            result->vt = VT_BSTR;
            result->bstrVal = SysAllocString(v->second.c_str());
            return S_OK;
        }
        return DISP_E_MEMBERNOTFOUND;
    }

private:
    static std::vector<std::wstring>& name_table() {
        static std::vector<std::wstring> table;
        return table;
    }
    ULONG references_ = 1;
};

struct DispatchCacheAccess : SapGuiObject {
    using SapGuiObject::clear_dispid_cache;
};

// Fresh DISPID cache, bulk state and GetObjectTree support around every test that talks to Node.
struct CleanState {
    CleanState() { reset(); }
    ~CleanState() { reset(); }
    static void reset() {
        DispatchCacheAccess::clear_dispid_cache();
        ComGuiSession::reset_object_tree_support();
        reset_bulk_reader_state_for_testing();
        _putenv_s("FAIRYFLY_SCREEN_READER", "");
        _putenv_s("FAIRYFLY_PROBE_ALL", "");
    }
};

// ---------------------------------------------------------------------------------------------
// Scene: wnd[0] -> usr (GuiUserArea) -> button, credential NAME/VALUE pair, password field,
// labelled check box, labelled plain field. The COM objects and the GetObjectTree answer are
// generated from the same table, so legacy and bulk read the same screen.
// ---------------------------------------------------------------------------------------------

constexpr const char* kWnd = "/app/con[0]/ses[0]/wnd[0]";
constexpr const char* kUsr = "/app/con[0]/ses[0]/wnd[0]/usr";

struct Spec {
    std::string leaf;  // id below usr
    std::string type;
    std::string name;
    std::string text;
    std::map<std::string, std::string> extra;  // tree-only/COM properties: AccTooltip, LeftLabel, ...
    std::optional<bool> changeable;
    std::optional<bool> selected;
    std::string acc_label;
};

std::vector<Spec> scene_specs() {
    std::vector<Spec> specs;
    specs.push_back({"btnSAVE", "GuiButton", "SAVE", "Save", {{"AccTooltip", "Save it"}}, false, {}, ""});
    specs.push_back({"txtNAME", "GuiTextField", "NAME", "API_PASSWORD", {}, false, {}, ""});
    specs.push_back({"txtVALUE", "GuiTextField", "VALUE", "hunter2-secret", {}, true, {}, ""});
    specs.push_back({"pwdLOGIN", "GuiPasswordField", "LOGIN", "swordfish-secret", {}, true, {}, ""});
    specs.push_back({"chkFLAG", "GuiCheckBox", "FLAG", "", {{"LeftLabel", std::string(kUsr) + "/lblFLAG"}},
                     true, true, ""});
    specs.push_back({"lblFLAG", "GuiLabel", "FLAG_LBL", "Flag label", {}, {}, {}, ""});
    specs.push_back({"txtPLAIN", "GuiTextField", "PLAIN", "abc", {{"DefaultTooltip", "Plain field"}}, true, {}, "Plain"});
    return specs;
}

std::string spec_id(const Spec& spec) { return std::string(kUsr) + "/" + spec.leaf; }

std::string scene_tree_answer(const std::vector<Spec>& specs) {
    json usr_children = json::array();
    for (const auto& spec : specs) {
        json props = {{"Id", spec_id(spec)}, {"Type", spec.type}, {"Name", spec.name},
                      {"Text", spec.text}, {"DisplayedText", spec.text}};
        if (spec.changeable) props["Changeable"] = *spec.changeable ? "true" : "false";
        for (const auto& [key, value] : spec.extra) props[key] = value;
        usr_children.push_back({{"properties", props}, {"children", json::array()}});
    }
    json usr = {{"properties", {{"Id", kUsr}, {"Type", "GuiUserArea"}, {"Name", "usr"}, {"Text", ""}}},
                {"children", usr_children}};
    json window = {{"properties", {{"Id", kWnd}, {"Type", "GuiMainWindow"}, {"Name", "wnd[0]"}, {"Text", "Scene"}}},
                   {"children", json::array({usr})}};
    return json{{"children", json::array({window})}}.dump();
}

std::wstring widen(const std::string& text) { return std::wstring(text.begin(), text.end()); }

struct Scene {
    std::vector<Node*> owned;
    Node *session = nullptr, *window = nullptr, *usr = nullptr;
    std::map<std::string, Node*> by_leaf;
    std::vector<Spec> specs = scene_specs();

    Node* make(const std::string& type, const std::string& id) {
        auto* node = new Node();
        node->strings[L"Type"] = widen(type);
        node->strings[L"Id"] = widen(id);
        node->unknown_names = {L"Enabled", L"Visible"};
        owned.push_back(node);
        return node;
    }

    Scene() {
        session = make("GuiSession", "/app/con[0]/ses[0]");
        auto* info = make("GuiSessionInfo", "");
        info->strings[L"Transaction"] = L"ZTEST";
        window = make("GuiMainWindow", kWnd);
        window->strings[L"Text"] = L"Scene";
        window->strings[L"Name"] = L"wnd[0]";
        usr = make("GuiUserArea", kUsr);
        usr->strings[L"Name"] = L"usr";
        usr->strings[L"Text"] = L"";
        window->items = {usr};
        auto* window_children = make("GuiCollection", "");
        window_children->items = {usr};
        window->dispatches[L"Children"] = window_children;
        auto* usr_children = make("GuiCollection", "");
        for (const auto& spec : specs) {
            auto* node = make(spec.type, spec_id(spec));
            node->strings[L"Name"] = widen(spec.name);
            node->strings[L"Text"] = widen(spec.text);
            node->strings[L"DisplayedText"] = widen(spec.text);
            if (spec.changeable) node->bools[L"Changeable"] = *spec.changeable;
            if (spec.selected) node->bools[L"Selected"] = *spec.selected;
            if (!spec.acc_label.empty()) node->strings[L"AccLabel"] = widen(spec.acc_label);
            for (const auto& [key, value] : spec.extra) {
                if (key != "LeftLabel") node->strings[widen(key)] = widen(value);
            }
            node->dispatches[L"Parent"] = usr;
            by_leaf[spec.leaf] = node;
            usr_children->items.push_back(node);
            usr->find_by_id[widen(spec.leaf)] = node;
            session->find_by_id[widen(spec_id(spec))] = node;
        }
        by_leaf["chkFLAG"]->dispatches[L"LeftLabel"] = by_leaf["lblFLAG"];
        usr->dispatches[L"Children"] = usr_children;
        session->dispatches[L"ActiveWindow"] = window;
        session->dispatches[L"Info"] = info;
        session->bools[L"Busy"] = false;
        session->find_by_id[widen(kWnd)] = window;
        session->find_by_id[widen(kUsr)] = usr;
        const auto answer = scene_tree_answer(specs);
        session->object_tree_payload = widen(answer);
    }
    ~Scene() { for (auto* node : owned) node->Release(); }
    ComGuiSessionPtr wrapper() { return ComGuiSession::create(IDispatchPtr(session)); }
    void set_tree_answer(const std::string& answer) { session->object_tree_payload = widen(answer); }
};

// Result of one read with the legacy reader and what the scripted source saw.
struct ScriptedSource {
    std::atomic<int> calls{0};
    std::vector<std::string> ids;
    std::vector<std::vector<std::string>> props;
    std::function<std::optional<std::string>()> answer;

    ObjectTreeSource source() {
        return [this](const std::string& id, const std::vector<std::string>& requested) {
            ++calls;
            ids.push_back(id);
            props.push_back(requested);
            return answer();
        };
    }
};

json elements_of(const Result& result) { return result.data.at("elements"); }

Result read_with_mode(Scene& scene, std::optional<TreeReaderMode> mode, ScriptedSource* source = nullptr,
                      bool include_structure = true, bool probe_all = false) {
    ScreenReader reader(scene.wrapper());
    reader.set_tree_reader_mode(mode);
    reader.set_probe_all(probe_all);
    if (source) reader.set_object_tree_source(source->source());
    return reader.read(include_structure);
}

}  // namespace

// =============================================================================================
// (a) Parity with the recorded legacy reads
// =============================================================================================

TEST_CASE("bulk plain elements equal the legacy elements of the recorded screens", "[bulk][parity]") {
    size_t compared_total = 0;
    for (const char* screen : kScreens) {
        DYNAMIC_SECTION(screen) {
            const json tree_file = json::parse(read_fixture(std::string(screen) + ".tree.json"));
            const json legacy = json::parse(read_fixture(std::string(screen) + ".legacy.json"));
            const std::string window_id = tree_file.at("id").get<std::string>();

            std::string error;
            auto tree = parse_object_tree(tree_file.at("tree").dump(), window_id, &error);
            REQUIRE(tree.has_value());
            TreeSnapshot snapshot(std::move(*tree));
            // The window's child count of the legacy read equals the tree's (the bulk read checks it).
            REQUIRE(snapshot.root().children.size() == legacy.at("child_count").get<size_t>());

            std::map<std::string, json> legacy_by_id;
            for (const auto& element : legacy.at("elements")) legacy_by_id[element.at("id")] = element;

            // AccLabel is read per element: the fixtures answer "" (labels come from the tree);
            // Selected comes from the recorded legacy element.
            std::vector<std::string> probed_ids;
            ElementProbeSource probe = [&](const std::string& id, bool want_label, bool want_selected) {
                probed_ids.push_back(id);
                ElementProbe answer;
                if (want_label) answer.acc_label = "";
                if (want_selected) {
                    const auto it = legacy_by_id.find(id);
                    answer.selected = it != legacy_by_id.end() && it->second.value("selected", false);
                }
                return answer;
            };

            ScreenElementCollector collector;
            replay_discovery(snapshot, window_id, false, collector);

            json built = json::array();
            for (const auto& id : collector.element_ids()) {
                if (collector.is_grid_id(id)) continue;  // phase 2B grids/tables stay on the legacy path
                const ObjectTreeNode* node = snapshot.find(id);
                REQUIRE(node != nullptr);
                if (is_phase3_skipped_container(node->type, id)) continue;
                if (auto element = build_bulk_plain_element(snapshot, *node, probe))
                    built.push_back(std::move(*element));
            }
            // The unchanged tail of ScreenReader::read.
            ScreenReader::collapse_label_duplicates(built);

            // Legacy elements the bulk builder is responsible for: plain types, no grid/tree
            // payload. Everything else (GuiShell in all subtypes, grids, trees, tabular user
            // areas, unknown types) is read element by element in both modes.
            json expected = json::array();
            for (const auto& element : legacy.at("elements")) {
                if (!is_bulk_plain_type(element.value("type", ""))) continue;
                if (element.contains("table_data") || element.contains("tree_data") ||
                    element.contains("grid_data")) continue;
                expected.push_back(element);
            }
            // The tail also redacts report labels; the fixture is post-redaction.
            redact_sensitive_report_labels(built);

            REQUIRE(built.size() == expected.size());
            for (size_t i = 0; i < expected.size(); ++i) {
                INFO(screen << " element " << i << " " << expected[i].value("id", ""));
                INFO("bulk:   " << built[i].dump());
                INFO("legacy: " << expected[i].dump());
                REQUIRE(built[i] == expected[i]);
            }
            compared_total += built.size();
            CHECK(built.size() > 0);
        }
    }
    CHECK(compared_total > 0);
}

// =============================================================================================
// (e) Targeted reads and the request
// =============================================================================================

TEST_CASE("bulk reads AccLabel only for input types and Selected only for check and radio",
          "[bulk][probe]") {
    const json answer = json::parse(R"({"children":[{"properties":{"Id":"w","Type":"GuiMainWindow"},
      "children":[{"properties":{"Id":"w/usr","Type":"GuiUserArea"},"children":[
        {"properties":{"Id":"w/usr/btn","Type":"GuiButton","Text":"b"}},
        {"properties":{"Id":"w/usr/lbl","Type":"GuiLabel","Text":"l"}},
        {"properties":{"Id":"w/usr/txt","Type":"GuiTextField","Text":"t","DisplayedText":"t","Changeable":"true"}},
        {"properties":{"Id":"w/usr/ctxt","Type":"GuiCTextField","Text":"t","DisplayedText":"t","Changeable":"true"}},
        {"properties":{"Id":"w/usr/pwd","Type":"GuiPasswordField","Text":"x"}},
        {"properties":{"Id":"w/usr/cmb","Type":"GuiComboBox","Text":"c","DisplayedText":"c","Changeable":"true"}},
        {"properties":{"Id":"w/usr/chk","Type":"GuiCheckBox","Text":"k","Changeable":"true"}},
        {"properties":{"Id":"w/usr/rad","Type":"GuiRadioButton","Text":"r","Changeable":"true"}},
        {"properties":{"Id":"w/usr/ok","Type":"GuiOkCodeField","Text":"","Changeable":"true"}},
        {"properties":{"Id":"w/usr/tab","Type":"GuiTab","Text":"tab"}}]}]}]})");
    auto tree = parse_object_tree(answer.dump(), "w");
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));

    struct Call { bool label; bool selected; };
    std::map<std::string, Call> calls;
    ElementProbeSource probe = [&](const std::string& id, bool want_label, bool want_selected) {
        calls[id] = {want_label, want_selected};
        ElementProbe result;
        result.selected = true;
        return result;
    };
    for (const char* leaf : {"btn", "lbl", "txt", "ctxt", "pwd", "cmb", "chk", "rad", "ok", "tab"}) {
        const auto* node = snapshot.find(std::string("w/usr/") + leaf);
        REQUIRE(node != nullptr);
        REQUIRE(build_bulk_plain_element(snapshot, *node, probe).has_value());
    }
    for (const char* none : {"btn", "lbl", "ok", "tab"})
        CHECK(calls.count(std::string("w/usr/") + none) == 0);
    for (const char* input : {"txt", "ctxt", "pwd", "cmb", "chk", "rad"}) {
        INFO(input);
        REQUIRE(calls.count(std::string("w/usr/") + input) == 1);
        CHECK(calls[std::string("w/usr/") + input].label);
    }
    CHECK_FALSE(calls["w/usr/txt"].selected);
    CHECK_FALSE(calls["w/usr/pwd"].selected);
    CHECK(calls["w/usr/chk"].selected);
    CHECK(calls["w/usr/rad"].selected);
    // One FindById read per element at most: label and selected are asked for together.
    CHECK(calls.size() == 6);
}

TEST_CASE("bulk label comes from the label element in the tree when AccLabel is empty",
          "[bulk][probe]") {
    const json answer = json::parse(R"({"children":[{"properties":{"Id":"w","Type":"GuiMainWindow"},
      "children":[
        {"properties":{"Id":"w/lblA","Type":"GuiLabel","Text":"Left caption"}},
        {"properties":{"Id":"w/lblB","Type":"GuiLabel","Text":"Right caption"}},
        {"properties":{"Id":"w/lblE","Type":"GuiLabel","Text":""}},
        {"properties":{"Id":"w/txtL","Type":"GuiTextField","Text":"v","DisplayedText":"v","Changeable":"true","LeftLabel":"w/lblA","RightLabel":"w/lblB"}},
        {"properties":{"Id":"w/txtR","Type":"GuiTextField","Text":"v","DisplayedText":"v","Changeable":"true","LeftLabel":"w/lblE","RightLabel":"w/lblB"}},
        {"properties":{"Id":"w/txtAcc","Type":"GuiTextField","Text":"v","DisplayedText":"v","Changeable":"true","LeftLabel":"w/lblA"}},
        {"properties":{"Id":"w/txtGone","Type":"GuiTextField","Text":"v","DisplayedText":"v","Changeable":"true","LeftLabel":"w/elsewhere"}}]}]})");
    auto tree = parse_object_tree(answer.dump(), "w");
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));
    ElementProbeSource probe = [](const std::string& id, bool, bool) {
        ElementProbe result;
        if (id == "w/txtAcc") result.acc_label = "Accessibility";
        return result;
    };
    const auto label_of = [&](const char* id) {
        const auto built = build_bulk_plain_element(snapshot, *snapshot.find(id), probe);
        return built ? std::optional<std::string>(built->value("label", "<none>")) : std::nullopt;
    };
    CHECK(label_of("w/txtL") == "Left caption");
    CHECK(label_of("w/txtR") == "Right caption");   // empty left label falls through to the right one
    CHECK(label_of("w/txtAcc") == "Accessibility");  // AccLabel wins
    CHECK_FALSE(label_of("w/txtGone").has_value());  // label outside the snapshot: legacy read
}

TEST_CASE("the object tree request never contains Selected or AccLabel", "[bulk][probe]") {
    CleanState state;
    Scene scene;
    ScriptedSource source;
    source.answer = [&] { return std::optional<std::string>(scene_tree_answer(scene.specs)); };
    const Result result = read_with_mode(scene, TreeReaderMode::Bulk, &source);
    REQUIRE(result.status == Result::Status::Success);
    REQUIRE(source.calls == 1);
    CHECK(source.ids.at(0) == kWnd);
    const auto& props = source.props.at(0);
    CHECK(props == object_tree_requested_properties());
    for (const auto& name : props) {
        CHECK(name != "Selected");
        CHECK(name != "AccLabel");
    }
}

// =============================================================================================
// Whole reads on the scripted scene
// =============================================================================================

TEST_CASE("bulk read equals the legacy read of the same scene", "[bulk][reader]") {
    CleanState state;
    Scene scene;
    const Result legacy = read_with_mode(scene, TreeReaderMode::Legacy);
    REQUIRE(legacy.status == Result::Status::Success);
    const int legacy_tree_calls = scene.session->object_tree_calls;
    CHECK(legacy_tree_calls == 0);

    SECTION("through the default GetObjectTree source and COM probe") {
        for (auto* node : scene.owned) node->reads.clear();
        const Result bulk = read_with_mode(scene, TreeReaderMode::Bulk);
        REQUIRE(bulk.status == Result::Status::Success);
        CHECK(scene.session->object_tree_calls == 1);
        CHECK(bulk.data == legacy.data);
        CHECK(bulk.diagnostics.at("screen_reader").at("used") == "bulk");
        CHECK(bulk.diagnostics.at("screen_reader").at("mode") == "bulk");
        CHECK_FALSE(bulk.diagnostics.at("screen_reader").contains("fallback_reason"));
        // The tree replaced the per-element reads of the plain elements.
        CHECK(scene.by_leaf["btnSAVE"]->reads[L"Text"] == 0);
        CHECK(scene.by_leaf["txtPLAIN"]->reads[L"DisplayedText"] == 0);
        CHECK(scene.by_leaf["txtPLAIN"]->reads[L"AccLabel"] == 1);  // the one targeted read
        CHECK(scene.by_leaf["chkFLAG"]->reads[L"Selected"] == 1);
    }
    SECTION("auto mode is the same read") {
        const Result bulk = read_with_mode(scene, TreeReaderMode::Auto);
        REQUIRE(bulk.status == Result::Status::Success);
        CHECK(bulk.data == legacy.data);
        CHECK(bulk.diagnostics.at("screen_reader").at("used") == "bulk");
    }
    SECTION("the output holds no credential text") {
        const Result bulk = read_with_mode(scene, TreeReaderMode::Bulk);
        const std::string dump = bulk.data.dump();
        CHECK(dump.find("hunter2") == std::string::npos);
        CHECK(dump.find("swordfish") == std::string::npos);
        const json elements = elements_of(bulk);
        bool found_value = false;
        bool found_password = false;
        for (const auto& element : elements) {
            if (element.value("name", "") == "VALUE") {
                found_value = true;
                CHECK(is_redaction_marker(element.at("text").get<std::string>()));
                CHECK(element.at("text") == redaction_marker(redaction_reason::paired_name));
            }
            if (element.value("name", "") == "LOGIN") {
                found_password = true;
                CHECK(element.at("text") == redaction_marker(redaction_reason::password_field));
            }
        }
        CHECK(found_value);
        CHECK(found_password);
    }
    SECTION("labels and selection survive") {
        const Result bulk = read_with_mode(scene, TreeReaderMode::Bulk);
        bool checked = false;
        for (const auto& element : elements_of(bulk)) {
            if (element.value("name", "") == "FLAG") {
                checked = true;
                CHECK(element.at("label") == "Flag label");
                CHECK(element.at("selected") == true);
            }
        }
        CHECK(checked);
    }
}

TEST_CASE("the legacy path is untouched when the mode is Legacy", "[bulk][reader]") {
    CleanState state;
    Scene scene;
    ScriptedSource source;
    source.answer = [] { return std::optional<std::string>("never used"); };

    // No override and no environment variable: Legacy. No diagnostics, no tree call.
    const Result by_default = read_with_mode(scene, std::nullopt, &source);
    const Result explicit_legacy = read_with_mode(scene, TreeReaderMode::Legacy, &source);
    REQUIRE(by_default.status == Result::Status::Success);
    CHECK(source.calls == 0);
    CHECK(scene.session->object_tree_calls == 0);
    CHECK(by_default.diagnostics.empty());
    CHECK(explicit_legacy.diagnostics.empty());
    CHECK(by_default.data == explicit_legacy.data);

    // An unknown environment value is Legacy as well.
    _putenv_s("FAIRYFLY_SCREEN_READER", "turbo");
    const Result unknown = read_with_mode(scene, std::nullopt, &source);
    CHECK(source.calls == 0);
    CHECK(unknown.data == by_default.data);
    CHECK(unknown.diagnostics.empty());
}

TEST_CASE("FAIRYFLY_SCREEN_READER selects the mode", "[bulk][mode]") {
    CleanState state;
    CHECK(tree_reader_mode_from_environment() == TreeReaderMode::Legacy);
    _putenv_s("FAIRYFLY_SCREEN_READER", "bulk");
    CHECK(tree_reader_mode_from_environment() == TreeReaderMode::Bulk);
    _putenv_s("FAIRYFLY_SCREEN_READER", "AUTO");
    CHECK(tree_reader_mode_from_environment() == TreeReaderMode::Auto);
    _putenv_s("FAIRYFLY_SCREEN_READER", "legacy");
    CHECK(tree_reader_mode_from_environment() == TreeReaderMode::Legacy);
    _putenv_s("FAIRYFLY_SCREEN_READER", "nonsense");
    CHECK(tree_reader_mode_from_environment() == TreeReaderMode::Legacy);

    CHECK(parse_tree_reader_mode("bulk") == TreeReaderMode::Bulk);
    CHECK(parse_tree_reader_mode("") == std::nullopt);
    CHECK(std::string(tree_reader_mode_name(TreeReaderMode::Auto)) == "auto");

    // The environment drives a reader without an explicit override (MCP reads rely on this).
    Scene scene;
    _putenv_s("FAIRYFLY_SCREEN_READER", "auto");
    const Result viaEnvironment = read_with_mode(scene, std::nullopt);
    CHECK(viaEnvironment.diagnostics.at("screen_reader").at("used") == "bulk");
    // An explicit override wins over the environment.
    const Result overridden = read_with_mode(scene, TreeReaderMode::Legacy);
    CHECK(overridden.diagnostics.empty());
}

// =============================================================================================
// (b) Fallback matrix
// =============================================================================================

TEST_CASE("auto mode falls back to the legacy reader for this read", "[bulk][fallback]") {
    CleanState state;
    Scene scene;
    const Result legacy = read_with_mode(scene, TreeReaderMode::Legacy);
    REQUIRE(legacy.status == Result::Status::Success);

    ScriptedSource source;
    std::string expected_reason;

    SECTION("no answer") {
        source.answer = [] { return std::optional<std::string>(); };
        expected_reason = "no_answer";
    }
    SECTION("invalid JSON") {
        source.answer = [] { return std::optional<std::string>("{not json"); };
        expected_reason = "invalid_tree";
    }
    SECTION("wrong root id") {
        source.answer = [] {
            return std::optional<std::string>(
                R"({"children":[{"properties":{"Id":"/other","Type":"GuiMainWindow"},"children":[]}]})");
        };
        expected_reason = "invalid_tree";
    }
    SECTION("a thrown exception") {
        source.answer = []() -> std::optional<std::string> { throw std::runtime_error("boom"); };
        expected_reason = "exception";
    }
    SECTION("child count that disagrees with the window") {
        source.answer = [] {
            return std::optional<std::string>(json{{"children", json::array({json{
                {"properties", {{"Id", kWnd}, {"Type", "GuiMainWindow"}}},
                {"children", json::array()}}})}}.dump());
        };
        expected_reason = "node_count_mismatch";
    }
    SECTION("an unsupported GuiSession.GetObjectTree") {
        scene.session->unknown_names.insert(L"GetObjectTree");
        const Result result = read_with_mode(scene, TreeReaderMode::Auto);
        REQUIRE(result.status == Result::Status::Success);
        CHECK(result.data == legacy.data);
        CHECK(result.diagnostics.at("screen_reader").at("used") == "legacy");
        CHECK(result.diagnostics.at("screen_reader").at("fallback_reason") == "unsupported");
        CHECK(ComGuiSession::object_tree_support() == ObjectTreeSupport::Missing);
        CHECK_FALSE(bulk_reader_disabled());
        return;
    }

    const Result result = read_with_mode(scene, TreeReaderMode::Auto, &source);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(source.calls == 1);
    CHECK(result.data == legacy.data);
    const auto& info = result.diagnostics.at("screen_reader");
    CHECK(info.at("mode") == "auto");
    CHECK(info.at("used") == "legacy");
    CHECK(info.at("fallback_reason").get<std::string>().rfind(expected_reason, 0) == 0);
    CHECK_FALSE(bulk_reader_disabled());
}

TEST_CASE("a fallback reason never carries payload text", "[bulk][fallback][privacy]") {
    CleanState state;
    Scene scene;
    ScriptedSource source;
    source.answer = [] { return std::optional<std::string>(R"({"children": "hunter2-secret"})"); };
    const Result result = read_with_mode(scene, TreeReaderMode::Auto, &source);
    REQUIRE(result.status == Result::Status::Success);
    CHECK(result.diagnostics.dump().find("hunter2") == std::string::npos);
    CHECK(result.data.dump().find("hunter2") == std::string::npos);
}

TEST_CASE("a server fault disables the bulk path for the process", "[bulk][fallback]") {
    CleanState state;
    Scene scene;
    const Result legacy = read_with_mode(scene, TreeReaderMode::Legacy);

    SECTION("raised by an injected source") {
        ScriptedSource source;
        source.answer = []() -> std::optional<std::string> { throw ObjectTreeServerFault(); };
        const Result first = read_with_mode(scene, TreeReaderMode::Auto, &source);
        REQUIRE(first.status == Result::Status::Success);
        CHECK(first.data == legacy.data);
        CHECK(first.diagnostics.at("screen_reader").at("fallback_reason") == "server_fault");
        CHECK(bulk_reader_disabled());
        CHECK(source.calls == 1);

        const Result later = read_with_mode(scene, TreeReaderMode::Auto, &source);
        CHECK(source.calls == 1);  // the source is not called again
        CHECK(later.data == legacy.data);
        CHECK(later.diagnostics.at("screen_reader").at("fallback_reason") == "disabled");

        reset_bulk_reader_state_for_testing();
        CHECK_FALSE(bulk_reader_disabled());
    }
    SECTION("RPC_E_SERVERFAULT from the real GetObjectTree call") {
        scene.session->object_tree_hresult = RPC_E_SERVERFAULT;
        const Result first = read_with_mode(scene, TreeReaderMode::Auto);
        REQUIRE(first.status == Result::Status::Success);
        CHECK(first.data == legacy.data);
        CHECK(first.diagnostics.at("screen_reader").at("fallback_reason") == "server_fault");
        CHECK(bulk_reader_disabled());
        CHECK(scene.session->object_tree_calls == 1);
        (void)read_with_mode(scene, TreeReaderMode::Auto);
        CHECK(scene.session->object_tree_calls == 1);
    }
    SECTION("any other COM failure only falls back") {
        scene.session->object_tree_hresult = E_FAIL;
        const Result first = read_with_mode(scene, TreeReaderMode::Auto);
        CHECK(first.data == legacy.data);
        CHECK(first.diagnostics.at("screen_reader").at("fallback_reason") == "no_answer");
        CHECK_FALSE(bulk_reader_disabled());
    }
    SECTION("a disabled bulk path in forced mode is an error") {
        scene.session->object_tree_hresult = RPC_E_SERVERFAULT;
        (void)read_with_mode(scene, TreeReaderMode::Auto);
        const Result forced = read_with_mode(scene, TreeReaderMode::Bulk);
        CHECK(forced.status == Result::Status::Error);
        CHECK(forced.error.at("code") == "OBJECT_TREE_UNAVAILABLE");
        CHECK(forced.error.at("reason") == "disabled");
    }
}

TEST_CASE("three consecutive validator failures disable the bulk path", "[bulk][fallback]") {
    CleanState state;
    Scene scene;
    const Result legacy = read_with_mode(scene, TreeReaderMode::Legacy);
    bool valid = false;
    ScriptedSource source;
    source.answer = [&] {
        return std::optional<std::string>(valid ? scene_tree_answer(scene.specs) : "garbage");
    };

    SECTION("three in a row") {
        for (int i = 0; i < 3; ++i) {
            CHECK_FALSE(bulk_reader_disabled());
            const Result failed = read_with_mode(scene, TreeReaderMode::Auto, &source);
            CHECK(failed.data == legacy.data);
        }
        CHECK(bulk_reader_disabled());
        const Result after = read_with_mode(scene, TreeReaderMode::Auto, &source);
        CHECK(source.calls == 3);
        CHECK(after.data == legacy.data);
        CHECK(after.diagnostics.at("screen_reader").at("fallback_reason") == "disabled");
    }
    SECTION("a success resets the streak") {
        (void)read_with_mode(scene, TreeReaderMode::Auto, &source);
        (void)read_with_mode(scene, TreeReaderMode::Auto, &source);
        valid = true;
        const Result good = read_with_mode(scene, TreeReaderMode::Auto, &source);
        CHECK(good.diagnostics.at("screen_reader").at("used") == "bulk");
        valid = false;
        (void)read_with_mode(scene, TreeReaderMode::Auto, &source);
        (void)read_with_mode(scene, TreeReaderMode::Auto, &source);
        CHECK_FALSE(bulk_reader_disabled());
    }
}

TEST_CASE("forced Bulk reports OBJECT_TREE_UNAVAILABLE instead of falling back", "[bulk][fallback]") {
    CleanState state;
    Scene scene;
    ScriptedSource source;

    SECTION("no answer") {
        source.answer = [] { return std::optional<std::string>(); };
        const Result result = read_with_mode(scene, TreeReaderMode::Bulk, &source);
        REQUIRE(result.status == Result::Status::Error);
        CHECK(result.error.at("code") == "OBJECT_TREE_UNAVAILABLE");
        CHECK(result.error.at("reason") == "no_answer");
        CHECK(result.error.at("message").get<std::string>().find("no_answer") != std::string::npos);
        CHECK(result.data.empty());
    }
    SECTION("invalid tree") {
        source.answer = [] { return std::optional<std::string>("[]"); };
        const Result result = read_with_mode(scene, TreeReaderMode::Bulk, &source);
        REQUIRE(result.status == Result::Status::Error);
        CHECK(result.error.at("code") == "OBJECT_TREE_UNAVAILABLE");
    }
    SECTION("exception") {
        source.answer = []() -> std::optional<std::string> { throw std::runtime_error("boom"); };
        const Result result = read_with_mode(scene, TreeReaderMode::Bulk, &source);
        REQUIRE(result.status == Result::Status::Error);
        CHECK(result.error.at("reason") == "exception");
    }
    SECTION("success is a normal read") {
        source.answer = [&] { return std::optional<std::string>(scene_tree_answer(scene.specs)); };
        const Result result = read_with_mode(scene, TreeReaderMode::Bulk, &source);
        CHECK(result.status == Result::Status::Success);
    }
}

TEST_CASE("requests that cannot use the tree stay on the legacy reader", "[bulk][fallback]") {
    CleanState state;
    Scene scene;
    const Result legacy = read_with_mode(scene, TreeReaderMode::Legacy);
    ScriptedSource source;
    source.answer = [&] { return std::optional<std::string>(scene_tree_answer(scene.specs)); };

    SECTION("--probe-all") {
        const Result result = read_with_mode(scene, TreeReaderMode::Auto, &source, true, true);
        CHECK(source.calls == 0);
        CHECK(result.diagnostics.at("screen_reader").at("used") == "legacy");
        CHECK(result.diagnostics.at("screen_reader").at("fallback_reason") == "probe_all");
        // Forced bulk does not turn that into an error: the request asked for exhaustive probing.
        const Result forced = read_with_mode(scene, TreeReaderMode::Bulk, &source, true, true);
        CHECK(source.calls == 0);
        CHECK(forced.status == Result::Status::Success);
    }
    SECTION("FAIRYFLY_PROBE_ALL=1") {
        _putenv_s("FAIRYFLY_PROBE_ALL", "1");
        const Result result = read_with_mode(scene, TreeReaderMode::Auto, &source);
        CHECK(source.calls == 0);
        CHECK(result.diagnostics.at("screen_reader").at("fallback_reason") == "probe_all");
    }
    SECTION("include_structure=false") {
        const Result result = read_with_mode(scene, TreeReaderMode::Auto, &source, false);
        CHECK(source.calls == 0);
        REQUIRE(result.status == Result::Status::Success);
        CHECK_FALSE(result.data.contains("elements"));
        CHECK(result.diagnostics.at("screen_reader").at("used") == "legacy");
        const Result forced = read_with_mode(scene, TreeReaderMode::Bulk, &source, false);
        CHECK(source.calls == 0);
        CHECK(forced.status == Result::Status::Success);
    }
    SECTION("screen find") {
        ScreenReader reader(scene.wrapper());
        reader.set_tree_reader_mode(TreeReaderMode::Bulk);
        reader.set_object_tree_source(source.source());
        ScreenFindOptions query;
        query.type = "GuiButton";
        const Result found = reader.find(query);
        CHECK(found.status == Result::Status::Success);
        CHECK(source.calls == 0);
    }
    SECTION("the tree is dropped after the read") {
        // Nothing of the raw answer reaches the result besides the redacted elements.
        const Result result = read_with_mode(scene, TreeReaderMode::Bulk, &source);
        REQUIRE(result.status == Result::Status::Success);
        CHECK(result.diagnostics.dump().find("hunter2") == std::string::npos);
        CHECK(result.to_json().dump().find("swordfish") == std::string::npos);
    }
}

// =============================================================================================
// (d) Redaction parity for tree input
// =============================================================================================

TEST_CASE("credential fields stay redacted when the text comes from the tree", "[bulk][privacy]") {
    const json answer = json::parse(R"({"children":[{"properties":{"Id":"w","Type":"GuiMainWindow"},
      "children":[{"properties":{"Id":"w/usr","Type":"GuiUserArea"},"children":[
        {"properties":{"Id":"w/usr/pwdX","Type":"GuiPasswordField","Text":"swordfish","DisplayedText":"swordfish"}},
        {"properties":{"Id":"w/usr/txtPASSWORD","Type":"GuiTextField","Name":"PASSWORD","Text":"s3cret","DisplayedText":"s3cret","Changeable":"true"}},
        {"properties":{"Id":"w/usr/txtNEUTRAL","Type":"GuiTextField","Name":"NEUTRAL","Text":"s3cret","DisplayedText":"s3cret","Changeable":"true","LeftLabel":"w/usr/lblPW"}},
        {"properties":{"Id":"w/usr/lblPW","Type":"GuiLabel","Name":"PW","Text":"Password"}},
        {"properties":{"Id":"w/usr/txtPASSWORD_EXT_PWD_STATE","Type":"GuiTextField","Name":"S","Text":"Password set","DisplayedText":"Password set","Changeable":"false"}},
        {"properties":{"Id":"w/usr/txtPASSWORD_EXT_PWD_STATE2","Type":"GuiTextField","Name":"S2","Text":"Password set","DisplayedText":"Password set","Changeable":"true"}},
        {"properties":{"Id":"w/usr/txtUNKNOWN","Type":"GuiTextField","Name":"U","Text":"visible"}},
        {"properties":{"Id":"w/usr/lblSTRUCT","Type":"GuiLabel","Name":"L","Text":"password=hunter2","DisplayedText":"password=hunter2"}},
        {"properties":{"Id":"w/usr/txtOPT_NAME","Type":"GuiTextField","Name":"N","Text":"Authorization","DisplayedText":"Authorization","Changeable":"true"}},
        {"properties":{"Id":"w/usr/txtOPT_VALUE","Type":"GuiTextField","Name":"V","Text":"Bearer abc","DisplayedText":"Bearer abc","Changeable":"true"}},
        {"properties":{"Id":"w/usr/txtB_NAME","Type":"GuiTextField","Name":"BN","Text":"Content-Type","DisplayedText":"Content-Type","Changeable":"true"}},
        {"properties":{"Id":"w/usr/txtB_VALUE","Type":"GuiTextField","Name":"BV","Text":"text/plain","DisplayedText":"text/plain","Changeable":"true"}}
      ]}]}]})");
    auto tree = parse_object_tree(answer.dump(), "w");
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));
    ElementProbeSource probe = [](const std::string&, bool, bool) { return ElementProbe{}; };
    const auto text_of = [&](const char* leaf) {
        const auto* node = snapshot.find(std::string("w/usr/") + leaf);
        REQUIRE(node != nullptr);
        const auto built = build_bulk_plain_element(snapshot, *node, probe);
        REQUIRE(built.has_value());
        return built->at("text").get<std::string>();
    };
    // The raw password text is erased by the parser before anything can read it.
    CHECK(snapshot.find("w/usr/pwdX")->prop("Text").empty());
    CHECK(text_of("pwdX") == redaction_marker(redaction_reason::password_field));
    CHECK(text_of("txtPASSWORD") == redaction_marker(redaction_reason::password_name));
    CHECK(is_redaction_marker(text_of("txtNEUTRAL")));  // neutral id, credential label
    CHECK(text_of("txtPASSWORD_EXT_PWD_STATE") == "Password set");  // display-only state label
    CHECK(is_redaction_marker(text_of("txtPASSWORD_EXT_PWD_STATE2")));  // changeable: stays redacted
    CHECK(text_of("txtUNKNOWN") == "visible");
    CHECK(text_of("lblSTRUCT").find("hunter2") == std::string::npos);
    // VALUE fields follow their NAME sibling in the snapshot.
    CHECK(text_of("txtOPT_VALUE") == redaction_marker(redaction_reason::paired_name));
    CHECK(text_of("txtB_VALUE") == "text/plain");
}

TEST_CASE("a VALUE field without a verifiable parent is redacted", "[bulk][privacy]") {
    // Root of the snapshot has no parent: the sibling names cannot be observed.
    const json answer = json::parse(R"({"children":[{"properties":{"Id":"w/txtX_VALUE","Type":"GuiTextField",
        "Name":"V","Text":"leak","DisplayedText":"leak","Changeable":"true"},"children":[]}]})");
    auto tree = parse_object_tree(answer.dump(), "w/txtX_VALUE");
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));
    ElementProbeSource probe = [](const std::string&, bool, bool) { return ElementProbe{}; };
    const auto built = build_bulk_plain_element(snapshot, snapshot.root(), probe);
    REQUIRE(built.has_value());
    CHECK(built->at("text") == redaction_marker(redaction_reason::unverified));
}

TEST_CASE("elements the tree cannot describe go through the legacy extractor", "[bulk][fallback]") {
    CHECK_FALSE(is_bulk_plain_type("GuiShell"));
    CHECK_FALSE(is_bulk_plain_type("GuiGridView"));
    CHECK_FALSE(is_bulk_plain_type("GuiTableControl"));
    CHECK_FALSE(is_bulk_plain_type("GuiCustomControl"));
    CHECK_FALSE(is_bulk_plain_type("GuiSomethingNew"));
    for (const char* type : {"GuiButton", "GuiTextField", "GuiCTextField", "GuiPasswordField", "GuiLabel",
                             "GuiCheckBox", "GuiRadioButton", "GuiComboBox", "GuiComboBoxControl",
                             "GuiOkCodeField", "GuiTab", "GuiTabStrip", "GuiBox", "GuiUserArea",
                             "GuiSimpleContainer", "GuiScrollContainer", "GuiSubScreen", "GuiToolbar",
                             "GuiToolbarControl", "GuiMenubar", "GuiMenu", "GuiTitlebar",
                             "GuiStatusbar", "GuiStatusPane"})
        CHECK(is_bulk_plain_type(type));

    const json answer = json::parse(R"({"children":[{"properties":{"Id":"w","Type":"GuiMainWindow"},
      "children":[{"properties":{"Id":"w/shell","Type":"GuiShell","SubType":"Toolbar"}}]}]})");
    auto tree = parse_object_tree(answer.dump(), "w");
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));
    ElementProbeSource probe = [](const std::string&, bool, bool) { return ElementProbe{}; };
    CHECK_FALSE(build_bulk_plain_element(snapshot, *snapshot.find("w/shell"), probe).has_value());
}

TEST_CASE("children ids follow the legacy container rule and the 50 cap", "[bulk][parity]") {
    json menu_children = json::array();
    for (int i = 0; i < 60; ++i)
        menu_children.push_back({{"properties", {{"Id", "w/mbar/menu[0]/menu[" + std::to_string(i) + "]"},
                                                  {"Type", "GuiMenu"}, {"Text", "m"}}}});
    const json answer = {{"children", json::array({json{
        {"properties", {{"Id", "w"}, {"Type", "GuiMainWindow"}}},
        {"children", json::array({
            json{{"properties", {{"Id", "w/mbar/menu[0]"}, {"Type", "GuiMenu"}, {"Text", "M"}}},
                 {"children", menu_children}},
            json{{"properties", {{"Id", "w/usr"}, {"Type", "GuiUserArea"}}},
                 {"children", json::array({json{{"properties", {{"Id", "w/usr/btn"}, {"Type", "GuiButton"}}}}})}},
            json{{"properties", {{"Id", "w/tbar[0]"}, {"Type", "GuiToolbar"}}}}})}}})}};
    auto tree = parse_object_tree(answer.dump(), "w");
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));
    ElementProbeSource probe = [](const std::string&, bool, bool) { return ElementProbe{}; };

    const auto menu = build_bulk_plain_element(snapshot, *snapshot.find("w/mbar/menu[0]"), probe);
    REQUIRE(menu.has_value());
    CHECK(menu->at("child_count") == 50);
    CHECK(menu->at("children").size() == 50);
    CHECK(menu->at("children")[0] == "w/mbar/menu[0]/menu[0]");
    CHECK(menu->at("container_type") == "container");

    const auto usr = build_bulk_plain_element(snapshot, *snapshot.find("w/usr"), probe);
    REQUIRE(usr.has_value());
    CHECK(usr->at("container_type") == "form");
    CHECK_FALSE(usr->contains("children"));  // GuiUserArea children are collected by the traversal

    const auto tbar = build_bulk_plain_element(snapshot, *snapshot.find("w/tbar[0]"), probe);
    REQUIRE(tbar.has_value());
    CHECK(tbar->at("container_type") == "toolbar");
    CHECK_FALSE(tbar->contains("children"));
}

// =============================================================================================
// `screen read --tab` (read_tab_content): one GetObjectTree(tab id) after the select
// =============================================================================================

namespace {

// The GetObjectTree answer of a node and its descendants, generated from the fake COM objects
// themselves so legacy and bulk read the same screen.
json tree_node_of(Node* node) {
    static const wchar_t* kProps[] = {L"Id", L"Type", L"Name", L"Text", L"DisplayedText", L"SubType"};
    json props = json::object();
    for (const wchar_t* name : kProps) {
        const auto it = node->strings.find(name);
        if (it == node->strings.end()) continue;
        std::string value;
        for (wchar_t ch : it->second) value.push_back(static_cast<char>(ch));
        props[std::string(name, name + wcslen(name))] = value;
    }
    if (const auto it = node->bools.find(L"Changeable"); it != node->bools.end())
        props["Changeable"] = it->second ? "true" : "false";
    json children = json::array();
    if (const auto it = node->dispatches.find(L"Children"); it != node->dispatches.end())
        for (auto* child : static_cast<Node*>(it->second)->items)
            children.push_back(tree_node_of(static_cast<Node*>(child)));
    return {{"properties", props}, {"children", children}};
}

std::string tree_answer_of(Node* node) { return json{{"children", json::array({tree_node_of(node)})}}.dump(); }

constexpr const char* kTabStrip = "/app/con[0]/ses[0]/wnd[0]/usr/tabsSTRIP";
constexpr const char* kTabA = "/app/con[0]/ses[0]/wnd[0]/usr/tabsSTRIP/tabpA";
constexpr const char* kTabB = "/app/con[0]/ses[0]/wnd[0]/usr/tabsSTRIP/tabpB";
constexpr const char* kTabC = "/app/con[0]/ses[0]/wnd[0]/usr/tabsSTRIP/tabpC";

// wnd[0] -> usr -> strip(tabpA selected, tabpB with fields, tabpC empty)
struct TabScene2 {
    std::vector<Node*> owned;
    Node *session, *window, *usr, *strip, *tab_a, *tab_b, *tab_c, *field_a, *field_b, *button_b;
    int restored_a = 0;

    Node* make(const std::string& type, const std::string& id, const std::string& name = "",
               const std::string& text = "") {
        auto* node = new Node();
        node->strings[L"Type"] = widen(type);
        node->strings[L"Id"] = widen(id);
        node->strings[L"Name"] = widen(name);
        node->strings[L"Text"] = widen(text);
        node->unknown_names = {L"Enabled", L"Visible"};
        owned.push_back(node);
        return node;
    }
    Node* collection(std::vector<IDispatch*> items) {
        auto* node = make("GuiCollection", "");
        node->items = std::move(items);
        return node;
    }
    TabScene2() {
        session = make("GuiSession", "/app/con[0]/ses[0]");
        auto* info = make("GuiSessionInfo", "");
        info->strings[L"Transaction"] = L"SU01";
        window = make("GuiMainWindow", kWnd, "wnd[0]", "Maintain Users");
        usr = make("GuiUserArea", kUsr, "usr");
        strip = make("GuiTabStrip", kTabStrip, "tabsSTRIP");
        tab_a = make("GuiTab", kTabA, "tabpA", "Address");
        tab_b = make("GuiTab", kTabB, "tabpB", "Roles");
        tab_c = make("GuiTab", kTabC, "tabpC", "Empty");
        field_a = make("GuiTextField", std::string(kTabA) + "/txtA", "A", "alpha");
        field_a->strings[L"DisplayedText"] = L"alpha";
        field_a->bools[L"Changeable"] = true;
        field_b = make("GuiTextField", std::string(kTabB) + "/txtB", "B", "bravo");
        field_b->strings[L"DisplayedText"] = L"bravo";
        field_b->bools[L"Changeable"] = true;
        button_b = make("GuiButton", std::string(kTabB) + "/btnB", "B_BTN", "Go");
        tab_a->dispatches[L"Children"] = collection({field_a});
        tab_b->dispatches[L"Children"] = collection({field_b, button_b});
        tab_c->dispatches[L"Children"] = collection({});
        strip->dispatches[L"Children"] = collection({tab_a, tab_b, tab_c});
        strip->dispatches[L"SelectedTab"] = tab_a;
        usr->dispatches[L"Children"] = collection({strip});
        window->dispatches[L"Children"] = collection({usr});
        session->dispatches[L"ActiveWindow"] = window;
        session->dispatches[L"Info"] = info;
        session->bools[L"Busy"] = false;
        for (Node* node : {window, usr, strip, tab_a, tab_b, tab_c, field_a, field_b, button_b})
            session->find_by_id[node->strings[L"Id"]] = node;
        for (Node* tab : {tab_b, tab_c})
            tab->on_select = [this, tab] { strip->dispatches[L"SelectedTab"] = tab; };
        tab_a->on_select = [this] { ++restored_a; strip->dispatches[L"SelectedTab"] = tab_a; };
    }
    ~TabScene2() { for (auto* node : owned) node->Release(); }
    ComGuiSessionPtr wrapper() { return ComGuiSession::create(IDispatchPtr(session)); }

    // Answers GetObjectTree for any node of the scene by id.
    ObjectTreeSource tree_source(std::vector<std::string>* requested = nullptr) {
        return [this, requested](const std::string& id, const std::vector<std::string>&) {
            if (requested) requested->push_back(id);
            const auto it = session->find_by_id.find(widen(id));
            if (it == session->find_by_id.end()) return std::optional<std::string>();
            return std::optional<std::string>(tree_answer_of(static_cast<Node*>(it->second)));
        };
    }
};

Result read_tab_with(TabScene2& scene, const char* tab, TreeReaderMode mode, ObjectTreeSource source = nullptr) {
    ScreenReader reader(scene.wrapper());
    reader.set_tree_reader_mode(mode);
    if (source) reader.set_object_tree_source(std::move(source));
    return reader.read_tab(tab);
}

}  // namespace

TEST_CASE("read_tab uses one GetObjectTree call for the tab subtree", "[bulk][tab]") {
    CleanState state;
    TabScene2 scene;
    const Result legacy = read_tab_with(scene, "tabpB", TreeReaderMode::Legacy);
    REQUIRE(legacy.status == Result::Status::Success);
    CHECK(legacy.diagnostics.empty());
    REQUIRE(legacy.data.at("tabs_content").size() == 1);
    scene.restored_a = 0;
    scene.strip->dispatches[L"SelectedTab"] = scene.tab_a;

    std::vector<std::string> requested;
    const Result bulk = read_tab_with(scene, "tabpB", TreeReaderMode::Auto, scene.tree_source(&requested));
    REQUIRE(bulk.status == Result::Status::Success);
    CHECK(bulk.data == legacy.data);
    CHECK(requested == std::vector<std::string>{kTabB});
    CHECK(bulk.diagnostics.at("screen_reader").at("used") == "bulk");
    CHECK(scene.restored_a == 1);  // the previously selected tab is restored as before

    // The tab content really came from the tree: the field's own properties were not read again.
    scene.field_b->reads.clear();
    (void)read_tab_with(scene, "tabpB", TreeReaderMode::Bulk, scene.tree_source());
    CHECK(scene.field_b->reads[L"DisplayedText"] == 0);
    CHECK(scene.field_b->reads[L"Text"] == 0);
}

TEST_CASE("read_tab falls back per read and keeps the restore guard", "[bulk][tab][fallback]") {
    CleanState state;
    TabScene2 scene;
    const Result legacy = read_tab_with(scene, "tabpB", TreeReaderMode::Legacy);
    REQUIRE(legacy.status == Result::Status::Success);
    scene.strip->dispatches[L"SelectedTab"] = scene.tab_a;

    SECTION("no answer in auto mode") {
        scene.restored_a = 0;
        const Result result = read_tab_with(scene, "tabpB", TreeReaderMode::Auto,
            [](const std::string&, const std::vector<std::string>&) { return std::optional<std::string>(); });
        REQUIRE(result.status == Result::Status::Success);
        CHECK(result.data == legacy.data);
        CHECK(result.diagnostics.at("screen_reader").at("used") == "legacy");
        CHECK(result.diagnostics.at("screen_reader").at("fallback_reason") == "no_answer");
        CHECK(scene.restored_a == 1);
    }
    SECTION("invalid tree in auto mode") {
        const Result result = read_tab_with(scene, "tabpB", TreeReaderMode::Auto,
            [](const std::string&, const std::vector<std::string>&) { return std::optional<std::string>("{}"); });
        REQUIRE(result.status == Result::Status::Success);
        CHECK(result.data == legacy.data);
    }
    SECTION("child count that disagrees with the tab") {
        const Result result = read_tab_with(scene, "tabpB", TreeReaderMode::Auto,
            [&](const std::string& id, const std::vector<std::string>&) {
                return std::optional<std::string>(json{{"children", json::array({json{
                    {"properties", {{"Id", id}, {"Type", "GuiTab"}}}, {"children", json::array()}}})}}.dump());
            });
        CHECK(result.data == legacy.data);
        CHECK(result.diagnostics.at("screen_reader").at("fallback_reason") == "node_count_mismatch");
    }
    SECTION("a server fault disables the path and the tab is still read") {
        const Result result = read_tab_with(scene, "tabpB", TreeReaderMode::Auto,
            [](const std::string&, const std::vector<std::string>&) -> std::optional<std::string> {
                throw ObjectTreeServerFault();
            });
        CHECK(result.data == legacy.data);
        CHECK(bulk_reader_disabled());
    }
    SECTION("forced Bulk reports OBJECT_TREE_UNAVAILABLE and still restores the tab") {
        scene.restored_a = 0;
        const Result result = read_tab_with(scene, "tabpB", TreeReaderMode::Bulk,
            [](const std::string&, const std::vector<std::string>&) { return std::optional<std::string>(); });
        REQUIRE(result.status == Result::Status::Error);
        CHECK(result.error.at("code") == "OBJECT_TREE_UNAVAILABLE");
        CHECK(result.error.at("reason") == "no_answer");
        CHECK(scene.restored_a == 1);
    }
    SECTION("probe-all stays legacy") {
        ScreenReader reader(scene.wrapper());
        reader.set_tree_reader_mode(TreeReaderMode::Auto);
        reader.set_probe_all(true);
        int calls = 0;
        reader.set_object_tree_source([&](const std::string&, const std::vector<std::string>&) {
            ++calls;
            return std::optional<std::string>();
        });
        const Result result = reader.read_tab("tabpB");
        CHECK(calls == 0);
        CHECK(result.status == Result::Status::Success);
    }
}

TEST_CASE("read_tab fallback to the window user area uses GetObjectTree of wnd/usr", "[bulk][tab]") {
    CleanState state;
    TabScene2 scene;
    const Result legacy = read_tab_with(scene, "tabpC", TreeReaderMode::Legacy);
    REQUIRE(legacy.status == Result::Status::Success);
    scene.strip->dispatches[L"SelectedTab"] = scene.tab_a;
    scene.restored_a = 0;

    std::vector<std::string> requested;
    const Result bulk = read_tab_with(scene, "tabpC", TreeReaderMode::Bulk, scene.tree_source(&requested));
    REQUIRE(bulk.status == Result::Status::Success);
    CHECK(bulk.data == legacy.data);
    CHECK(requested == std::vector<std::string>{kTabC, kUsr});
    CHECK(scene.restored_a == 1);
}

TEST_CASE("read_with_tabs reads each tab through the tree and keeps the base read", "[bulk][tab]") {
    CleanState state;
    TabScene2 scene;
    const auto run = [&](TreeReaderMode mode, ObjectTreeSource source) {
        scene.strip->dispatches[L"SelectedTab"] = scene.tab_a;
        ScreenReader reader(scene.wrapper());
        reader.set_tree_reader_mode(mode);
        if (source) reader.set_object_tree_source(std::move(source));
        return reader.read_with_tabs();
    };
    const Result legacy = run(TreeReaderMode::Legacy, nullptr);
    REQUIRE(legacy.status == Result::Status::Success);

    std::vector<std::string> requested;
    const Result bulk = run(TreeReaderMode::Auto, scene.tree_source(&requested));
    REQUIRE(bulk.status == Result::Status::Success);
    CHECK(bulk.data == legacy.data);
    // window (base read) + one call per tab (+ the user area for the empty tab)
    CHECK(requested.front() == kWnd);
    CHECK(std::count(requested.begin(), requested.end(), std::string(kTabA)) == 1);
    CHECK(std::count(requested.begin(), requested.end(), std::string(kTabB)) == 1);
    CHECK(bulk.diagnostics.at("screen_reader").at("used") != "legacy");
}

TEST_CASE("replay_subtree replays one element like the legacy traversal", "[bulk][tab]") {
    TabScene2 scene;
    const std::string answer = tree_answer_of(scene.tab_b);
    auto tree = parse_object_tree(answer, kTabB);
    REQUIRE(tree.has_value());
    TreeSnapshot snapshot(std::move(*tree));
    ScreenElementCollector collector;
    replay_subtree(snapshot, kTabB, false, collector);
    CHECK(collector.element_ids() ==
          std::vector<std::string>{kTabB, std::string(kTabB) + "/txtB", std::string(kTabB) + "/btnB"});
    ScreenElementCollector unknown;
    replay_subtree(snapshot, "/not/in/tree", false, unknown);
    CHECK(unknown.size() == 0);
}
