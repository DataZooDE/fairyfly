#include "include/object_tree_diag.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

namespace fairyfly::diag {

bool diag_enabled(const char* env_value) {
    return env_value != nullptr && std::strcmp(env_value, "1") == 0;
}

bool diag_enabled_from_environment() {
    char* value = nullptr;
    size_t length = 0;
    bool enabled = false;
    if (_dupenv_s(&value, &length, "FAIRYFLY_DIAG") == 0 && value) {
        enabled = diag_enabled(value);
        std::free(value);
    }
    return enabled;
}

namespace {
int count_children(const nlohmann::json& children) {
    int total = 0;
    if (!children.is_array()) return 0;
    for (const auto& node : children) {
        if (!node.is_object()) continue;
        ++total;
        if (auto it = node.find("children"); it != node.end()) total += count_children(*it);
    }
    return total;
}
} // namespace

int count_object_tree_nodes(const nlohmann::json& tree) {
    if (!tree.is_object()) return 0;
    auto it = tree.find("children");
    return it == tree.end() ? 0 : count_children(*it);
}

nlohmann::json build_object_tree_diagnostic(const std::string& id, const std::vector<std::string>& props,
                                            const ObjectTreeFetcher& fetch) {
    nlohmann::json out;
    out["id"] = id;
    out["props"] = props;
    const auto started = std::chrono::steady_clock::now();
    const auto answer = fetch(id, props);
    out["call_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now() - started).count();
    out["supported"] = answer.has_value();
    if (!answer) return out;
    out["bytes"] = answer->size();
    try {
        out["tree"] = nlohmann::json::parse(*answer);
        out["node_count"] = count_object_tree_nodes(out["tree"]);
    } catch (const nlohmann::json::exception& e) {
        out["raw"] = *answer;
        out["parse_error"] = e.what();
    }
    return out;
}

} // namespace fairyfly::diag
