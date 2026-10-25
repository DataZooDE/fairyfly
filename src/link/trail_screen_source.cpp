#include "include/com/control_views.h"
#include "include/com/tree_view.h"
#include "include/shell_kind.h"
#include "include/link/trail_screen_source.h"

#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "include/cli_handler.h"
#include "include/com/wrapper.h"
#include "include/object_tree.h"
#include "include/screenshot_handler.h"

namespace fairyfly::link::trail {

namespace {

using sap::ComGuiElementPtr;
using sap::ComGuiSession;
using sap::ComGuiSessionPtr;

constexpr int kMaxWalkNodes = 3000;
constexpr int kMaxWalkDepth = 40;

bool input_type(const std::string& type) {
    return type == "GuiTextField" || type == "GuiCTextField" || type == "GuiPasswordField" || type == "GuiComboBox";
}

/// A property read that tells a failure apart from 0 (SapGuiObject::get_int_property cannot).
std::optional<int> strict_int(const sap::SapGuiObject& o, const wchar_t* name) {
    _variant_t v;
    bool lookup_failed = false;
    const HRESULT hr = o.invoke_property_get(name, v, &lookup_failed);
    if (lookup_failed || FAILED(hr)) return std::nullopt;
    if (v.vt == VT_I4 || v.vt == VT_INT) return int(v.lVal);
    if (v.vt == VT_I2) return int(v.iVal);
    _variant_t c;
    if (FAILED(VariantChangeType(&c, &v, 0, VT_I4))) return std::nullopt;
    return int(c.lVal);
}

std::optional<Rect> strict_rect(const sap::SapGuiObject& o) {
    auto x = strict_int(o, L"ScreenLeft");
    auto y = strict_int(o, L"ScreenTop");
    auto w = strict_int(o, L"Width");
    auto h = strict_int(o, L"Height");
    if (!x || !y || !w || !h || *w <= 0 || *h <= 0) return std::nullopt;
    return Rect{*x, *y, *w, *h};
}

/// Containers whose content GetObjectTree does not list (object_tree.h is_phase3_skipped_container): their controls
/// are reached element by element.
bool control_host(const std::string& type) {
    return type == "GuiCustomControl" || type == "GuiContainerShell" || type == "GuiSplitterShell" || type == "GuiDockShell" ||
           type == "GuiGOSShell" || type == "GuiContainerCtrl" || type == "GuiSplitterContainer";
}

/// Columns of grids and column trees. A grid whose columns cannot be read is opaque (blurred as a whole): a grid
/// without any column does not exist.
void read_columns(Field& f, const ComGuiElementPtr& e, const std::string& subtype) {
    if (f.type != "GuiShell" || !e) return;
    if (sap::is_grid_shell(subtype)) {
        try {
            f.columns = as_grid(*e).get_column_order();
        } catch (const std::exception&) {
            f.columns.clear();
        }
        if (f.columns.empty()) f.opaque = true;
    } else if (subtype == "Tree") {
        try {
            f.columns = as_tree(*e).get_tree_column_names();
        } catch (const std::exception&) {
        }
    }
}

class ComScreenSource : public ScreenSource {
public:
    explicit ComScreenSource(std::function<ComGuiSessionPtr()> session) : session_(std::move(session)) {}

    ScreenState state(const std::function<bool(const Field&)>& needs_rect) override {
        ScreenState s;
        try {
            auto session = session_ ? session_() : nullptr;
            if (!session) return s;
            try {
                if (session->is_busy()) session->wait_for_completion(2000);
            } catch (const std::exception&) {
            }
            auto window = session->get_active_window();
            if (!window) return s;
            const std::string window_full = window->get_id();
            s.window_id = relative_id(window_full);
            try {
                s.title = window->get_title();
            } catch (const std::exception&) {
            }
            try {
                s.transaction = session->get_transaction_code();
            } catch (const std::exception&) {
            }
            s.window = strict_rect(*window);

            std::vector<std::string> full_ids;
            if (!from_tree(session, window_full, s, full_ids) && !from_walk(window, s, full_ids)) return s;
            // the rectangles of the fields to blur, read one by one (only a handful per screen)
            for (size_t i = 0; i < s.fields.size(); i++) {
                if (!needs_rect || !needs_rect(s.fields[i])) continue;
                try {
                    auto e = session->find_element_by_id(full_ids[i]);
                    if (e) s.fields[i].rect = strict_rect(*e);
                } catch (const std::exception&) {
                    // stays unknown: the screenshot is dropped
                }
            }
            s.complete = true;
        } catch (const std::exception& e) {
            spdlog::warn("audit trail: the SAP screen could not be read: {}", e.what());
            s.complete = false;
        }
        return s;
    }

    std::string capture_png(const std::string& window_id) override {
        try {
            auto session = session_ ? session_() : nullptr;
            if (!session) return {};
            auto window = session->get_active_window();
            // the rectangles were read from `window_id`: a popup that opened since then must not get them
            if (!window || relative_id(window->get_id()) != window_id) return {};
            auto bytes = sap::ScreenshotHandler::hard_copy_png(window);
            return std::string(bytes.begin(), bytes.end());
        } catch (const std::exception& e) {
            spdlog::warn("audit trail: screenshot failed: {}", e.what());
            return {};
        }
    }

    std::optional<std::string> value(const std::string& element_id, std::optional<int> row, const std::string& column) override {
        try {
            auto session = session_ ? session_() : nullptr;
            if (!session) return std::nullopt;
            auto e = session->find_element_by_id(element_id);
            if (!e) return std::nullopt;
            if (row) return as_grid(*e).get_cell_value(*row, column);
            const std::string type = e->get_type();
            if (type == "GuiPasswordField") return std::string();  // never read
            if (type == "GuiCheckBox" || type == "GuiRadioButton") {
                auto sel = e->get_selected();
                if (!sel) return std::nullopt;
                return std::string(*sel ? "X" : "");
            }
            if (type == "GuiComboBox") {
                auto r = e->read_string_property_strict(L"Value");
                if (r.status == sap::PropertyStatus::Ok) return r.value;
            }
            return e->get_text();
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }

private:
    /// One GetObjectTree call for the window (the properties the bulk reader uses; never Selected).
    bool from_tree(const ComGuiSessionPtr& session, const std::string& window_full, ScreenState& s, std::vector<std::string>& full_ids) {
        sap::ObjectTreeFailure failure = sap::ObjectTreeFailure::None;
        std::optional<std::string> raw;
        try {
            raw = session->get_object_tree(window_full, sap::object_tree_requested_properties(), &failure);
        } catch (const std::exception&) {
            return false;
        }
        if (!raw) return false;
        std::string why;
        auto tree = sap::parse_object_tree(*raw, window_full, &why);
        raw->clear();
        if (!tree) return false;
        sap::TreeSnapshot snap(std::move(*tree));
        std::vector<const sap::ObjectTreeNode*> stack{&snap.root()};
        while (!stack.empty()) {
            const sap::ObjectTreeNode* n = stack.back();
            stack.pop_back();
            Field f;
            f.id = relative_id(n->id);
            f.type = n->type;
            f.name = n->prop("Name");
            f.label = snap.label_text_for(*n);
            for (auto p : {"AccTooltip", "Tooltip", "DefaultTooltip"})
                if (f.tooltip.empty()) f.tooltip = n->prop(p);
            f.input = input_type(f.type);
            if (f.type != "GuiPasswordField") f.value = n->prop("Text");
            const bool host = (control_host(f.type) || sap::is_phase3_skipped_container(n->type, n->id)) && n->children.empty();
            if (f.type == "GuiShell") {
                try {
                    read_columns(f, session->find_element_by_id(n->id), n->prop("SubType"));
                } catch (const std::exception&) {
                    if (sap::is_grid_shell(n->prop("SubType"))) f.opaque = true;
                }
            }
            s.fields.push_back(std::move(f));
            full_ids.push_back(n->id);
            if (host && s.fields.back().type != "GuiShell") {
                // the tree stops at control containers: walk their content over COM; what cannot be read is blurred
                const size_t at = s.fields.size() - 1;
                int nodes = 0;
                bool ok = false;
                try {
                    auto e = session->find_element_by_id(n->id);
                    ok = e != nullptr;
                    const int count = ok ? e->get_child_count() : 0;
                    for (int i = 0; i < count && ok; i++) ok = walk(e->get_child(i), 1, s, full_ids, nodes);
                } catch (const std::exception&) {
                    ok = false;
                }
                if (!ok) s.fields[at].opaque = true;
            }
            for (auto it = n->children.rbegin(); it != n->children.rend(); ++it) stack.push_back(&*it);
        }
        return true;
    }

    /// One element and its descendants over COM (bounded). False when it could not be read completely.
    bool walk(const ComGuiElementPtr& e, int depth, ScreenState& s, std::vector<std::string>& full_ids, int& nodes) {
        if (!e) return false;
        if (++nodes > kMaxWalkNodes || depth > kMaxWalkDepth) return false;
        Field f;
        const std::string full = e->get_id();
        f.id = relative_id(full);
        f.type = e->get_type();
        try { f.name = e->get_name(); } catch (const std::exception&) {}
        try { f.label = e->get_label(); } catch (const std::exception&) {}
        try { f.tooltip = e->get_tooltip(); } catch (const std::exception&) {}
        f.input = input_type(f.type);
        if (f.input && f.type != "GuiPasswordField") {
            try { f.value = e->get_text(); } catch (const std::exception&) {}
        }
        if (f.type == "GuiShell") {
            std::string subtype;
            try { subtype = e->get_subtype(); } catch (const std::exception&) {}
            read_columns(f, e, subtype);
        }
        s.fields.push_back(std::move(f));
        full_ids.push_back(full);
        const int n = e->get_child_count();
        for (int i = 0; i < n; i++)
            if (!walk(e->get_child(i), depth + 1, s, full_ids, nodes)) return false;
        return true;
    }

    /// SAP GUI without GetObjectTree: element by element (bounded; a screen that does not fit is incomplete).
    bool from_walk(const sap::ComGuiWindowPtr& window, ScreenState& s, std::vector<std::string>& full_ids) {
        s.fields.clear();
        full_ids.clear();
        int nodes = 0;
        bool ok = true;
        try {
            const int n = window->get_child_count();
            for (int i = 0; i < n && ok; i++) ok = walk(window->get_child(i), 1, s, full_ids, nodes);
        } catch (const std::exception&) {
            ok = false;
        }
        if (!ok) {
            s.fields.clear();
            full_ids.clear();
        }
        return ok;
    }

    std::function<ComGuiSessionPtr()> session_;
};

}  // namespace

std::shared_ptr<ScreenSource> make_com_screen_source_for(std::function<std::shared_ptr<sap::ComGuiSession>()> session) {
    return std::make_shared<ComScreenSource>(std::move(session));
}

std::shared_ptr<ScreenSource> make_com_screen_source(std::function<cli::CommandHandler*()> peek) {
    return make_com_screen_source_for([peek = std::move(peek)]() -> ComGuiSessionPtr {
        cli::CommandHandler* handler = peek ? peek() : nullptr;
        return handler ? handler->attached_com_session() : nullptr;
    });
}

}  // namespace fairyfly::link::trail
