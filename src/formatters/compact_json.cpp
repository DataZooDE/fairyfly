#include "include/formatters/compact_json.h"

#include <string>
#include <unordered_set>

namespace fairyfly {
namespace formatters {

using nlohmann::json;

namespace {

bool is_always_kept(const std::string& key) {
    return key == "id" || key == "type" || key == "name";
}

bool is_droppable(const json& v) {
    if (v.is_null()) return true;
    if (v.is_string()) return v.get_ref<const std::string&>().empty();
    if (v.is_array()) return v.empty();
    if (v.is_boolean()) return !v.get<bool>();
    return false;
}

json compact_element(const json& elem) {
    if (!elem.is_object()) return elem;
    json out = json::object();
    for (const auto& [key, value] : elem.items()) {
        if (!is_always_kept(key) && is_droppable(value)) continue;
        if ((key == "children" || key == "toolbar_buttons") && value.is_array()) {
            json kids = json::array();
            for (const auto& child : value) kids.push_back(compact_element(child));
            out[key] = std::move(kids);
        } else {
            out[key] = value;
        }
    }
    return out;
}

void index_ids(const json& elements, std::unordered_set<std::string>& ids) {
    if (!elements.is_array()) return;
    for (const auto& elem : elements) {
        if (!elem.is_object()) continue;
        if (elem.contains("id") && elem["id"].is_string()) ids.insert(elem["id"].get<std::string>());
        if (elem.contains("children")) index_ids(elem["children"], ids);
        if (elem.contains("toolbar_buttons")) index_ids(elem["toolbar_buttons"], ids);
    }
}

// Replace `list` by ids; objects not yet in `ids` are compacted and appended to `sink`.
json to_ids(const json& list, std::unordered_set<std::string>& ids, json& sink) {
    json out = json::array();
    for (const auto& item : list) {
        if (item.is_string()) { out.push_back(item); continue; }
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) continue;
        const std::string id = item["id"].get<std::string>();
        if (ids.insert(id).second) sink.push_back(compact_element(item));
        out.push_back(id);
    }
    return out;
}

void convert_hierarchy(json& scope, std::unordered_set<std::string>& ids, json& sink) {
    if (!scope.contains("hierarchy") || !scope["hierarchy"].is_object()) return;
    for (auto& [group, list] : scope["hierarchy"].items()) {
        if (list.is_array()) list = to_ids(list, ids, sink);
    }
}

}  // namespace

json compact_screen_json(const json& data) {
    if (!data.is_object()) return data;
    if (data.value("hierarchy_format", "") == "ids") return data;

    json out = data;
    json elements = json::array();
    if (data.contains("elements") && data["elements"].is_array()) {
        for (const auto& elem : data["elements"]) elements.push_back(compact_element(elem));
    }
    std::unordered_set<std::string> ids;
    index_ids(elements, ids);

    convert_hierarchy(out, ids, elements);
    if (out.contains("tabs_content") && out["tabs_content"].is_array()) {
        for (auto& tab : out["tabs_content"]) {
            if (!tab.is_object()) continue;
            convert_hierarchy(tab, ids, elements);
            if (tab.contains("elements") && tab["elements"].is_array())
                tab["elements"] = to_ids(tab["elements"], ids, elements);
        }
    }

    if (data.contains("elements") || !elements.empty()) out["elements"] = std::move(elements);
    out["compact"] = true;
    out["hierarchy_format"] = "ids";
    out["tab_elements_format"] = "ids";
    return out;
}

}  // namespace formatters
}  // namespace fairyfly
