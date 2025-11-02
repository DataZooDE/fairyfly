// C++ test to match VBScript ground truth test
// Outputs same JSON format as test_base_objects.vbs for comparison

#include "include/com/wrapper.h"
#include "include/sap_gui_base.h"
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <iostream>
#include <exception>

using json = nlohmann::json;
using namespace fairyfly::sap;

json object_to_json(SapGuiObject* obj, const std::string& object_type) {
    json j;
    j["object_type"] = object_type;

    try {
        j["id"] = obj->get_id();
    } catch (...) {
        j["id"] = nullptr;
    }

    try {
        j["name"] = obj->get_name();
    } catch (...) {
        j["name"] = nullptr;
    }

    try {
        j["type"] = obj->get_type();
    } catch (...) {
        j["type"] = nullptr;
    }

    try {
        j["type_as_number"] = obj->get_type_as_number();
    } catch (...) {
        j["type_as_number"] = nullptr;
    }

    return j;
}

int main() {
    try {
        // Disable spdlog console output for clean JSON output
        spdlog::set_level(spdlog::level::off);

        // Initialize COM
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(hr) && hr != S_FALSE) {
            std::cerr << "ERROR: Failed to initialize COM" << std::endl;
            return 1;
        }

        json result;
        result["status"] = "success";
        result["test_name"] = "test_base_objects_cpp";
        result["description"] = "C++ implementation matching VBScript ground truth";

        json objects = json::array();

        // Get SAP GUI application
        auto app = ComGuiApplication::create();
        if (!app) {
            std::cerr << "ERROR: Cannot get SAP GUI application" << std::endl;
            result["status"] = "error";
            result["error"] = "Cannot get SAP GUI application";
            std::cout << result.dump(2) << std::endl;
            return 1;
        }

        // Add GuiApplication object
        objects.push_back(object_to_json(app.get(), "GuiApplication"));

        // Iterate through connections using new collection API
        auto connections_col = app->connections();
        for (auto& conn : connections_col) {
            // Add GuiConnection object
            objects.push_back(object_to_json(&conn, "GuiConnection"));

            // Iterate through sessions using new collection API
            auto sessions_col = conn.sessions();
            for (auto& sess : sessions_col) {
                // Add GuiSession object
                objects.push_back(object_to_json(&sess, "GuiSession"));

                // Get main window if available
                try {
                    auto elem = sess.find_element_by_id("wnd[0]");
                    if (elem) {
                        objects.push_back(object_to_json(elem.get(), "GuiFrameWindow"));

                        // Try to get window wrapper
                        auto wnd = ComGuiWindow::create(elem->get_dispatch());
                        if (wnd) {
                            // Get a few child elements if available
                            auto children_col = wnd->children();
                            int child_count = 0;
                            for (auto& child : children_col) {
                                if (child_count >= 5) break; // Limit to first 5 children

                                try {
                                    std::string child_type = child.get_type();
                                    objects.push_back(object_to_json(&child, child_type));
                                } catch (...) {
                                    // Skip if we can't get type
                                }
                                child_count++;
                            }
                        }
                    }
                } catch (...) {
                    // Window not available, skip
                }
            }
        }

        result["objects"] = objects;

        // Output JSON to match VBScript format
        std::cout << result.dump() << std::endl;

        CoUninitialize();
        return 0;

    } catch (const std::exception& e) {
        json error_result;
        error_result["status"] = "error";
        error_result["test_name"] = "test_base_objects_cpp";
        error_result["error"] = e.what();
        std::cout << error_result.dump(2) << std::endl;
        return 1;
    }
}
