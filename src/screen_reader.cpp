#include "include/screen_reader.h"
#include "include/element_metadata_extractor.h"
#include "include/screen_element_collector.h"
#include "include/constants.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <unordered_set>
#include <thread>

namespace fairyfly {
namespace sap {

using utils::TraceGuard;

ScreenReader::ScreenReader(ComGuiSessionPtr session) : session_(session) {
}

void ScreenReader::flatten_and_group_elements(const json& elements, json& grouped) {
    for (const auto& elem : elements) {
        std::string type = elem.value("type", "");
        std::string container = elem.value("container_type", "");
        std::string elem_id = elem.value("id", "unknown");
        spdlog::debug("flatten_and_group_elements: Processing element type={} id={}", type, elem_id);

        // Group by container type or element type
        if (container == "toolbar" || type == "GuiToolbar" || type == "GuiMenubar") {
            grouped["toolbar"].push_back(elem);
        } else if (type == "GuiShell") {
            // GuiShell with Toolbar subtype should be in toolbar section
            std::string subtype = elem.value("subtype", "");
            if (subtype == "Toolbar") {
                grouped["toolbar"].push_back(elem);
            } else {
                grouped["other"].push_back(elem);
            }
        } else if (type == "GuiButton") {
            // Split buttons: toolbar buttons vs inline buttons
            std::string id = elem.value("id", "");

            // Classify as toolbar button if:
            // 1. In /tbar/ path (standard toolbar)
            // 2. In /titl/ path (title bar buttons)
            // 3. In shell container (shell/btn[X] pattern)
            // 4. NOT in /usr/ (those are inline form buttons)

            bool is_toolbar_button = false;
            if (id.find("/usr/") == std::string::npos) {  // Not in user area
                if (id.find("/tbar[") != std::string::npos ||   // Standard toolbar (e.g., /tbar[0]/btn[1])
                    id.find("/titl") != std::string::npos ||    // Title bar
                    id.find("/shell/btn") != std::string::npos) { // Shell container
                    is_toolbar_button = true;
                }
            }

            if (is_toolbar_button) {
                spdlog::debug("Classifying as toolbar button: {}", id);
                grouped["buttons"].push_back(elem);  // Toolbar buttons
            } else {
                spdlog::debug("Classifying as inline button: {}", id);
                grouped["inline_buttons"].push_back(elem);  // Inline buttons
            }
        } else if (type == "GuiTextField" || type == "GuiCTextField" ||
                   type == "GuiPasswordField" || type == "GuiComboBox" ||
                   type == "GuiCheckBox" || type == "GuiRadioButton") {
            grouped["form_fields"].push_back(elem);
        } else if (type == "GuiLabel") {
            // Labels go to form_fields if they're next to input fields
            grouped["form_fields"].push_back(elem);
        } else if (container == "table" || container == "grid" ||
                   type == "GuiTableControl" || type == "GuiGridView" ||
                   (type == "GuiShell" && elem.value("subtype", "") == "GridView")) {
            grouped["tables"].push_back(elem);
        } else if (container == "tabs" || type == "GuiTabStrip" || type == "GuiTab") {
            grouped["tabs"].push_back(elem);
        } else {
            grouped["other"].push_back(elem);
        }

        // Recursively process children
        if (elem.contains("children") && elem["children"].is_array()) {
            int child_count = elem["children"].size();
            spdlog::debug("Processing {} children of element type={}", child_count, type);
            flatten_and_group_elements(elem["children"], grouped);
        }
    }
}

json ScreenReader::group_elements_by_container(const json& elements) {
    json grouped;
    grouped["toolbar"] = json::array();
    grouped["buttons"] = json::array();
    grouped["inline_buttons"] = json::array();
    grouped["form_fields"] = json::array();
    grouped["tables"] = json::array();
    grouped["tabs"] = json::array();
    grouped["other"] = json::array();

    flatten_and_group_elements(elements, grouped);

    // Deduplicate all groups by element ID
    auto deduplicate = [](json& arr) {
        std::unordered_set<std::string> seen_ids;
        json deduped = json::array();
        for (const auto& elem : arr) {
            std::string id = elem.value("id", "");
            if (!id.empty() && seen_ids.insert(id).second) {
                deduped.push_back(elem);
            }
        }
        arr = deduped;
    };

    deduplicate(grouped["toolbar"]);
    deduplicate(grouped["buttons"]);
    deduplicate(grouped["inline_buttons"]);
    deduplicate(grouped["form_fields"]);
    deduplicate(grouped["tables"]);
    deduplicate(grouped["tabs"]);
    deduplicate(grouped["other"]);

    // Remove empty groups
    if (grouped["toolbar"].empty()) grouped.erase("toolbar");
    if (grouped["buttons"].empty()) grouped.erase("buttons");
    if (grouped["inline_buttons"].empty()) grouped.erase("inline_buttons");
    if (grouped["form_fields"].empty()) grouped.erase("form_fields");
    if (grouped["tables"].empty()) grouped.erase("tables");
    if (grouped["tabs"].empty()) grouped.erase("tabs");
    if (grouped["other"].empty()) grouped.erase("other");

    return grouped;
}

void ScreenReader::traverse_element_tree(
    ComGuiElementPtr element,
    ScreenElementCollector& collector,
    int depth) {
    const int MAX_DEPTH = 15;  // Limit depth (SEGW needs depth 10+)

    if (!element || !session_ || depth >= MAX_DEPTH) {
        return;
    }

    try {
        std::string type = element->get_type();
        std::string elem_id = element->get_id();

        // Add current element to collector (with O(1) deduplication)
        // If already seen, skip processing to avoid duplicate work
        if (!collector.add(element)) {
            return;  // Already processed this element
        }

        spdlog::debug("{}[depth={}] {} ({})",
                     std::string(depth * 2, ' '), depth, elem_id, type);

        // Container types that have nested structure
        bool is_container = (type == "GuiContainerShell" ||
                           type == "GuiSplitterShell" ||
                           type == "GuiUserArea" ||
                           type == "GuiCustomControl" ||
                           type == "GuiToolbar" ||           // Toolbar contains buttons
                           type == "GuiToolbarControl" ||    // Toolbar control
                           type == "GuiTitlebar");           // Title bar may contain buttons

        if (!is_container) {
            return;  // Leaf element, no children to traverse
        }

        // Try Children collection first, but track if it actually works
        try {
            int child_count = element->get_child_count();
            if (child_count > 0) {
                for (int i = 0; i < child_count; ++i) {
                    try {
                        auto child = element->get_child(i);
                        if (child) {
                            traverse_element_tree(child, collector, depth + 1);
                        }
                    } catch (const SapGuiException&) {
                        // Child access failed, will fall back to ID-based
                    } catch (const ComException&) {
                        // Child access failed, will fall back to ID-based
                    } catch (const std::exception&) {
                        // Child access failed, will fall back to ID-based
                    }
                }
                // Don't return here - continue with ID-based discovery
                // SAP GUI can have elements accessible via FindById but not Children collection
            }
        } catch (const std::exception&) {
            // Children collection not available
        }

        spdlog::debug("{}[depth={}] Trying ID-based discovery for {}",
                      std::string(depth * 2, ' '), depth, elem_id);

        // For ALL container types (GuiUserArea, GuiContainerShell, GuiSplitterShell),
        // try common child patterns via FindById
        if (is_container) {
            // Try /shell child
            try {
                auto shell_child = session_->find_element_by_id(elem_id + "/shell");
                if (shell_child) {
                    traverse_element_tree(shell_child, collector, depth + 1);
                }
            } catch (const std::exception&) {
                // Shell child not found, continue
            }

            // Try /shellcont[N] children (up to MAX_SHELLCONT_CHILDREN)
            for (int i = 0; i < constants::MAX_SHELLCONT_CHILDREN; ++i) {
                try {
                    std::string child_id = elem_id + "/shellcont[" + std::to_string(i) + "]";
                    auto child = session_->find_element_by_id(child_id);
                    if (child) {
                        traverse_element_tree(child, collector, depth + 1);
                    }
                } catch (const std::exception&) {
                    break;  // No more shellcont children
                }
            }

            // Try /shellcont child (no index)
            try {
                auto shellcont_child = session_->find_element_by_id(elem_id + "/shellcont");
                if (shellcont_child) {
                    traverse_element_tree(shellcont_child, collector, depth + 1);
                }
            } catch (const std::exception&) {
                // Shellcont child not found, continue
            }

            // Special handling for GuiUserArea: probe for grid-positioned labels
            // Pattern: /usr/lbl[row,col]
            if (type == "GuiUserArea") {
                spdlog::debug("{}Probing GuiUserArea for grid labels", std::string(depth * 2, ' '));

                // Probe grid bounds (up to MAX_GRID_ROW x MAX_GRID_COL, stop early if consecutive misses)
                int consecutive_row_misses = 0;

                for (int row = 0; row < constants::MAX_GRID_ROW && consecutive_row_misses < constants::MAX_CONSECUTIVE_MISSES; ++row) {
                    int consecutive_col_misses = 0;
                    bool found_any_in_row = false;

                    for (int col = 0; col < constants::MAX_GRID_COL && consecutive_col_misses < constants::MAX_CONSECUTIVE_MISSES; ++col) {
                        try {
                            std::string label_id = elem_id + "/lbl[" + std::to_string(row) + "," + std::to_string(col) + "]";
                            auto label = session_->find_element_by_id(label_id);

                            if (label) {
                                collector.add(label);
                                found_any_in_row = true;
                                consecutive_col_misses = 0;
                            } else {
                                consecutive_col_misses++;
                            }
                        } catch (const std::exception&) {
                            consecutive_col_misses++;
                        }
                    }

                    if (!found_any_in_row) {
                        consecutive_row_misses++;
                    } else {
                        consecutive_row_misses = 0;
                    }
                }

                spdlog::debug("{}Grid probing complete for {}", std::string(depth * 2, ' '), elem_id);
            }
        }
    } catch (const SapGuiException& e) {
        spdlog::debug("traverse_element_tree: SapGuiException: {}", e.what());
        // Skip elements that throw exceptions during processing
    } catch (const ComException& e) {
        spdlog::debug("traverse_element_tree: ComException: {}", e.what());
        // Skip elements that throw exceptions during processing
    } catch (const std::exception& e) {
        spdlog::debug("traverse_element_tree: std::exception: {}", e.what());
        // Skip elements that throw exceptions during processing
    }
}

std::vector<ComGuiElementPtr> ScreenReader::discover_elements(ComGuiWindowPtr window) {
    ScreenElementCollector collector;
    collector.reserve(200);  // Reserve space for typical complex screens

    try {
        std::string window_id = window->get_id();
        int child_count = window->get_child_count();
        spdlog::debug("Window {} has {} top-level children, starting recursive traversal", window_id, child_count);

        // FIRST: Explicitly probe for toolbar and title bar (often missed by Children collection)
        // These are critical for button discovery
        try {
            // Try toolbar: wnd[N]/tbar[0], wnd[N]/tbar[1], etc.
            for (int i = 0; i < 3; ++i) {  // Most windows have 0-2 toolbars
                try {
                    std::string tbar_id = window_id + "/tbar[" + std::to_string(i) + "]";
                    auto tbar = session_->find_element_by_id(tbar_id);
                    if (tbar) {
                        spdlog::debug("Found toolbar via ID: {}", tbar_id);
                        traverse_element_tree(tbar, collector, 0);
                    }
                } catch (const std::exception&) {
                    break;  // No more toolbars
                }
            }
        } catch (const std::exception&) {
            // Toolbar probing failed
        }

        try {
            // Try title bar: wnd[N]/titl
            std::string titl_id = window_id + "/titl";
            auto titl = session_->find_element_by_id(titl_id);
            if (titl) {
                spdlog::debug("Found title bar via ID: {}", titl_id);
                traverse_element_tree(titl, collector, 0);
            }
        } catch (const std::exception&) {
            // Title bar not found
        }

        // SECOND: Recursively traverse all top-level children (covers usr, mbar, etc.)
        for (int i = 0; i < child_count; ++i) {
            try {
                auto child = window->get_child(i);
                if (child) {
                    traverse_element_tree(child, collector, 0);
                }
            } catch (const std::exception&) {
                // Skip problematic children
            }
        }
    } catch (const SapGuiException& e) {
        spdlog::warn("SapGuiException during element tree traversal: {}", e.what());
    } catch (const ComException& e) {
        spdlog::warn("ComException during element tree traversal: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("std::exception during element tree traversal: {}", e.what());
    }

    spdlog::info("Discovered {} unique elements via recursive traversal", collector.elements().size());
    return collector.elements();
}

Result ScreenReader::read(bool include_structure) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }

        auto window = session_->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }

        result.status = Result::Status::Success;
        result.data["screen_id"] = window->get_id();
        result.data["title"] = window->get_title();
        result.data["transaction"] = session_->get_transaction_code();
        result.data["child_count"] = window->get_child_count();

        if (include_structure) {
            // Clear extraction cache at start of each screen read
            ElementMetadataExtractor::clear_cache();

            // Discover all unique elements (O(1) deduplication)
            auto element_objects = discover_elements(window);

            // Extract metadata for each element
            json elements = json::array();
            for (const auto& elem : element_objects) {
                try {
                    json elem_data = ElementMetadataExtractor::extract(elem);
                    if (!elem_data.is_null()) {
                        elements.push_back(elem_data);
                    }
                } catch (const SapGuiException& e) {
                    spdlog::debug("SapGuiException extracting element metadata: {}", e.what());
                    // Skip elements with metadata extraction errors
                } catch (const ComException& e) {
                    spdlog::debug("ComException extracting element metadata: {}", e.what());
                    // Skip elements with metadata extraction errors
                } catch (const std::exception& e) {
                    spdlog::debug("std::exception extracting element metadata: {}", e.what());
                    // Skip elements with metadata extraction errors
                }
            }

            result.data["elements"] = elements;
            result.data["element_count"] = elements.size();

            // Group elements by type for better LLM understanding
            result.data["hierarchy"] = group_elements_by_container(elements);
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Read screen with {} elements (duration: {}ms)",
                     result.data.value("element_count", 0), result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Screen read failed: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

Result ScreenReader::read_with_tabs() {
    TraceGuard trace("ScreenReader::read_with_tabs");
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }

        // Get initial screen structure
        Result initial_result = read(true);
        if (initial_result.status != Result::Status::Success) {
            return initial_result;
        }

        json screen_data = initial_result.data;
        json tabs_content = json::array();

        // Find all GuiTab elements in the hierarchy
        std::vector<json> tab_elements;
        if (screen_data.contains("hierarchy") && screen_data["hierarchy"].contains("tabs")) {
            for (const auto& tab : screen_data["hierarchy"]["tabs"]) {
                std::string type = tab.value("type", "");
                if (type == "GuiTab") {
                    tab_elements.push_back(tab);
                }
            }
        }

        if (tab_elements.empty()) {
            spdlog::info("No tabs found in screen, returning normal screen read");
            return initial_result;
        }

        spdlog::info("Found {} tabs to expand", tab_elements.size());

        // For each tab, select it and capture content
        for (size_t i = 0; i < tab_elements.size(); ++i) {
            const auto& tab = tab_elements[i];
            std::string tab_id = tab.value("id", "");
            std::string tab_name = tab.value("text", "Unnamed Tab");

            try {
                spdlog::debug("Selecting tab {}/{}: {} [{}]", i + 1, tab_elements.size(), tab_name, tab_id);

                // Find and select the tab
                auto tab_elem = session_->find_element_by_id(tab_id);
                if (!tab_elem) {
                    spdlog::warn("Tab element not found: {}", tab_id);
                    continue;
                }

                // Select the tab (triggers server communication)
                tab_elem->select();

                // Wait for session to be ready (server response)
                int wait_attempts = 0;
                const int max_wait_ms = 5000;
                const int poll_interval_ms = 100;
                while (session_->is_busy() && wait_attempts < (max_wait_ms / poll_interval_ms)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
                    wait_attempts++;
                }

                if (session_->is_busy()) {
                    spdlog::warn("Timeout waiting for tab {} to load", tab_name);
                    continue;
                }

                // Small additional delay to ensure content is loaded
                std::this_thread::sleep_for(std::chrono::milliseconds(200));

                // Re-read screen to get tab content
                Result tab_result = read(true);
                if (tab_result.status != Result::Status::Success) {
                    spdlog::warn("Failed to read content for tab {}: {}",
                                   tab_name, tab_result.error.value("message", "unknown error"));
                    continue;
                }

                // Store tab data
                json tab_data;
                tab_data["tab_id"] = tab_id;
                tab_data["tab_name"] = tab_name;
                tab_data["tab_type"] = tab["type"];
                tab_data["tab_index"] = i;
                tab_data["elements"] = tab_result.data["elements"];
                tab_data["hierarchy"] = tab_result.data["hierarchy"];
                tab_data["element_count"] = tab_result.data.value("element_count", 0);

                tabs_content.push_back(tab_data);

                int elem_count = tab_data["element_count"].get<int>();
                spdlog::info("Captured tab {} with {} elements", tab_name, elem_count);

            } catch (const ComException& e) {
                spdlog::warn("Failed to expand tab {}: {}", tab_name, e.what());
                continue;
            } catch (const std::exception& e) {
                spdlog::warn("Exception expanding tab {}: {}", tab_name, e.what());
                continue;
            }
        }

        // Add tabs content to result
        screen_data["tabs_content"] = tabs_content;
        screen_data["tabs_expanded"] = true;
        screen_data["expanded_tab_count"] = tabs_content.size();

        result.status = Result::Status::Success;
        result.data = screen_data;

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Read screen with {} tabs expanded (duration: {}ms)",
                    tabs_content.size(), result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Screen read with tabs failed: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
        spdlog::error("Exception in read_with_tabs: {}", e.what());
    }

    return result;
}

} // namespace sap
} // namespace fairyfly

