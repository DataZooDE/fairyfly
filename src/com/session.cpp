#include "include/com/wrapper.h"
#include "include/com/wrapper_helpers.h"
#include "include/constants.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include <chrono>
#include <thread>

namespace fairyfly {
namespace sap {

// ============================================================================
// ComGuiSession Implementation
// ============================================================================

ComGuiSession::ComGuiSession(IDispatchPtr sess) : SapGuiObject(sess) {
    if (!sess) throw ComException("Invalid session pointer");
}

ComGuiSessionPtr ComGuiSession::create(IDispatchPtr sess) {
    return std::make_shared<ComGuiSession>(sess);
}

// get_id(), get_name(), get_type(), get_type_as_number() now inherited from SapGuiObject

bool ComGuiSession::is_busy() const {
    return get_bool_property(L"Busy");
}

bool ComGuiSession::is_alive() const {
    try {
        std::string id = get_id();
        return !id.empty();
    } catch (const std::exception& e) {
        spdlog::debug("is_alive check failed: {}", e.what());
        return false;
    }
}

ComGuiWindowPtr ComGuiSession::get_active_window() const {
    auto window = get_dispatch_property(L"ActiveWindow");
    if (!window) return nullptr;
    return ComGuiWindow::create(window);
}

ComGuiElementPtr ComGuiSession::find_element_by_id(const std::string& id) const {
    auto elem = ::fairyfly::sap::call_method_with_string(dispatch_, "FindById", id);
    if (!elem) {
        throw ComException("Element not found: " + id);
    }
    return ComGuiElement::create(elem);
}

void ComGuiSession::start_transaction(const std::string& tcode) {
    try {
        auto window = get_active_window();
        if (!window) throw ComException("No active window");

        auto tcode_elem = find_element_by_id("wnd[0]/tbar[0]/okcd");
        if (tcode_elem) {
            tcode_elem->set_text(tcode);
            spdlog::info("Started transaction: {}", tcode);
        }
    } catch (const ComException& e) {
        spdlog::error("Failed to start transaction '{}': {}", tcode, e.what());
        throw;
    }
}

void ComGuiSession::wait_for_completion(int timeout_ms) {
    auto start = std::chrono::high_resolution_clock::now();
    while (is_busy()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start
        );
        if (elapsed.count() > timeout_ms) {
            throw ComException("Session timeout waiting for completion");
        }
        std::this_thread::sleep_for(constants::Milliseconds(constants::SESSION_WAIT_INTERVAL_MS));
    }
}

void ComGuiSession::send_vkey(int vkey) {
    utils::TraceGuard trace("ComGuiSession::send_vkey");
    if (!dispatch_) throw ComException("Null session");

    try {
        _bstr_t method("SendVKey");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(dispatch_, method.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("SendVKey method not found: 0x{:08X}", hr));
            throw ComException("SendVKey method not found", hr);
        }

        _variant_t vkey_var(vkey);
        DISPPARAMS params = {(VARIANT*)&vkey_var, nullptr, 1, 0};
        _variant_t result;
        hr = dispatch_->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_METHOD,
                             &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            trace.mark_error(fmt::format("SendVKey invoke failed: 0x{:08X}", hr));
            throw ComException("Failed to send virtual key", hr);
        }

        trace.mark_success();
        spdlog::debug("Sent virtual key: {}", vkey);

        // Brief wait for server to process key
        std::this_thread::sleep_for(constants::Milliseconds(constants::SESSION_WAIT_INTERVAL_MS));
    } catch (const ComException&) {
        throw;
    } catch (const std::exception& e) {
        trace.mark_error(e.what());
        throw ComException(std::string("Exception in send_vkey: ") + e.what());
    }
}

ComGuiElementPtr ComGuiSession::wait_for_element(const std::string& element_id, int timeout_ms) {
    utils::TraceGuard trace("ComGuiSession::wait_for_element");
    if (!dispatch_) throw ComException("Null session");

    auto start = std::chrono::high_resolution_clock::now();
    int attempts = 0;

    while (true) {
        try {
            auto elem = find_element_by_id(element_id);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start
            );
            spdlog::debug("wait_for_element|found|element={}|elapsed_ms={}|attempts={}",
                         element_id, elapsed.count(), attempts);
            trace.mark_success();
            return elem;
        } catch (const ComException&) {
            attempts++;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - start
            );

            if (elapsed.count() > timeout_ms) {
                spdlog::warn("wait_for_element|timeout|element={}|elapsed_ms={}|attempts={}",
                           element_id, elapsed.count(), attempts);
                trace.mark_error("timeout");
                return nullptr;
            }

            spdlog::debug("wait_for_element|retry|element={}|attempt={}|elapsed_ms={}",
                         element_id, attempts, elapsed.count());

            // Progressive backoff: RETRY_DELAY_MS, 2*RETRY_DELAY_MS, 3*RETRY_DELAY_MS... up to MAX_RETRY_DELAY_MS
            int delay = (constants::RETRY_DELAY_MS * attempts < constants::MAX_RETRY_DELAY_MS) 
                       ? (constants::RETRY_DELAY_MS * attempts) 
                       : constants::MAX_RETRY_DELAY_MS;
            std::this_thread::sleep_for(constants::Milliseconds(delay));
        }
    }
}

std::string ComGuiSession::get_transaction_code() const {
    try {
        // Get the Info object from the session
        auto info = get_dispatch_property(L"Info");
        if (!info) {
            return "";
        }

        // Get the Transaction property from the Info object
        // We need to access a string property on the info IDispatch object
        _bstr_t prop_name("Transaction");
        DISPID dispid;
        HRESULT hr = get_dispid_via_typeinfo(info, prop_name.GetBSTR(), &dispid);
        if (FAILED(hr)) {
            return "";  // Transaction property not found
        }

        DISPPARAMS params = {nullptr, nullptr, 0, 0};
        _variant_t result;
        hr = info->Invoke(dispid, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYGET,
                         &params, &result, nullptr, nullptr);
        if (FAILED(hr)) {
            return "";  // Failed to get Transaction property
        }

        if (result.vt == VT_BSTR) {
            return std::string(_bstr_t(result.bstrVal));
        }
        return "";
    } catch (const ComException&) {
        // Transaction property not available or error accessing it
        return "";
    } catch (const std::exception&) {
        return "";
    }
}

} // namespace sap
} // namespace fairyfly

