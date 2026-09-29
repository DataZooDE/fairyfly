#pragma once

#include <optional>
#include <string>
#include <exception>

namespace fairyfly::sap {

template <typename GetId>
std::optional<int> find_collection_index_by_id(int count, const std::string& target_id,
                                                GetId&& get_id) {
    for (int index = 0; index < count; ++index) {
        try {
            if (get_id(index) == target_id) return index;
        } catch (const std::exception&) {
            // A stale item does not prevent a later live item from matching.
        }
    }
    return std::nullopt;
}

}  // namespace fairyfly::sap
