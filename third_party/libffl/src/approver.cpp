#include "approver.hpp"

#include <chrono>
#include <future>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ffy::link {

namespace {
struct Fixed : Approver {
    explicit Fixed(bool a) : a_(a) {}
    bool approve(const std::string&) override { return a_; }
    bool a_;
};
#ifdef _WIN32
struct Desktop : Approver {
    bool approve(const std::string& text) override {
        auto p = std::make_shared<std::promise<bool>>();
        auto f = p->get_future();
        std::thread([p, text] {
            int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
            std::wstring w(size_t(n), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, w.data(), n);
            int r = MessageBoxW(nullptr, w.c_str(), L"fairyfly AI access - confirm SAP change", MB_YESNO | MB_ICONQUESTION | MB_TOPMOST | MB_SETFOREGROUND);
            p->set_value(r == IDYES);
        }).detach();
        return f.wait_for(std::chrono::seconds(60)) == std::future_status::ready && f.get();
    }
};
#endif
}  // namespace

std::shared_ptr<Approver> fixed_approver(bool a) { return std::make_shared<Fixed>(a); }
std::shared_ptr<Approver> desktop_approver() {
#ifdef _WIN32
    return std::make_shared<Desktop>();
#else
    return std::make_shared<Fixed>(false);
#endif
}

}  // namespace ffy::link
