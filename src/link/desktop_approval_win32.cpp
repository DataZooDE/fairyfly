// Windows UI of the desktop approval (see the header): the toast, its COM activator for the notification centre, the
// approval window and the check for a locked or non-interactive session.
#include "include/link/desktop_approval_win32.h"

#include <windows.h>
#include <unknwn.h>
#include <commctrl.h>
#include <shobjidl_core.h>
#include <wtsapi32.h>
#include <NotificationActivationCallback.h>

#include <roapi.h>
#include <windows.data.xml.dom.h>
#include <windows.foundation.h>
#include <windows.ui.notifications.h>
#include <wrl/client.h>
#include <wrl/event.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <spdlog/spdlog.h>

#include <ctime>
#include <filesystem>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <thread>

#include "include/com/utf8.h"
#include "tray/tray_resource.h"

// The approval window is a task dialog: common controls 6 (fairyfly.exe has no manifest of its own).
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "runtimeobject.lib")

namespace fairyfly::link::approval {

namespace {

using com::utf8_to_wide;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;
namespace wn = ABI::Windows::UI::Notifications;
namespace xml = ABI::Windows::Data::Xml::Dom;
namespace wf = ABI::Windows::Foundation;
namespace fs = std::filesystem;

constexpr wchar_t kGroup[] = L"approval";
constexpr GUID kActivatorClsid = {0xDAAF1065, 0xEF17, 0x4CFF, {0xB3, 0xD2, 0xEF, 0xAF, 0x70, 0x59, 0x49, 0x59}};

std::wstring wide(const std::string& s) {
    try {
        return utf8_to_wide(s);
    } catch (const std::exception&) {
        return std::wstring(s.begin(), s.end());
    }
}

// ---- the notification's answers (the toast's Activated event and the COM activator) -------------------------------

std::mutex g_sink_mu;
std::function<void(const std::string&)> g_sink;

void deliver(const std::string& text) {
    std::function<void(const std::string&)> sink;
    {
        std::lock_guard<std::mutex> l(g_sink_mu);
        sink = g_sink;
    }
    if (sink) sink(text);
}

struct Activator : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, INotificationActivationCallback> {
    IFACEMETHODIMP Activate(LPCWSTR, LPCWSTR args, const NOTIFICATION_USER_INPUT_DATA*, ULONG) override {
        try {
            if (args) deliver(com::wide_to_utf8(args));
        } catch (...) {
        }
        return S_OK;
    }
};

struct ActivatorFactory : Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IClassFactory> {
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        auto a = Microsoft::WRL::Make<Activator>();
        return a ? a.CopyTo(iid, out) : E_OUTOFMEMORY;
    }
    IFACEMETHODIMP LockServer(BOOL) override { return S_OK; }
};

void set_value(HKEY root, const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    HKEY k = nullptr;
    if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), DWORD((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
}

fs::path module_dir() {
    std::wstring path(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), DWORD(path.size())));
    return fs::path(path).parent_path();
}

// Registers fairyfly for notifications (once per process): the AppUserModelID with its activator (also without the
// Start-menu entry, e.g. a portable copy), the activator's server (the tray starter; COM appends -Embedding) and the
// activator itself on a thread that keeps the multithreaded apartment alive for the toasts.
class ToastService {
public:
    static ToastService& get() {
        static ToastService s;
        return s;
    }
    bool ready() {
        std::call_once(once_, [this] { start(); });
        return ready_;
    }

private:
    void start() {
        const std::wstring aumid_key = std::wstring(L"Software\\Classes\\AppUserModelId\\") + kAppUserModelId;
        set_value(HKEY_CURRENT_USER, aumid_key, L"DisplayName", L"fairyfly");
        set_value(HKEY_CURRENT_USER, aumid_key, L"CustomActivator", kToastActivatorClsid);
        const fs::path starter = module_dir() / L"fairyfly-tray.exe";
        std::error_code ec;
        if (fs::exists(starter, ec))
            set_value(HKEY_CURRENT_USER, std::wstring(L"Software\\Classes\\CLSID\\") + kToastActivatorClsid + L"\\LocalServer32", nullptr,
                      L"\"" + starter.wstring() + L"\"");
        SetCurrentProcessExplicitAppUserModelID(kAppUserModelId);

        std::promise<bool> registered;
        auto result = registered.get_future();
        std::thread([p = std::move(registered)]() mutable {
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
                p.set_value(false);
                return;
            }
            DWORD cookie = 0;
            HRESULT hr = E_FAIL;
            if (auto factory = Microsoft::WRL::Make<ActivatorFactory>())
                hr = CoRegisterClassObject(kActivatorClsid, factory.Get(), CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE, &cookie);
            if (FAILED(hr)) spdlog::warn("approval: the notification centre cannot answer (CoRegisterClassObject 0x{:08X})", unsigned(hr));
            p.set_value(true);  // the toast's own Activated event still answers while it is on screen
            // keeps the apartment (and the activator) for the life of the process
            HANDLE never = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            WaitForSingleObject(never, INFINITE);
        }).detach();
        ready_ = result.get();
    }
    std::once_flag once_;
    bool ready_ = false;
};

// COM on this thread for WinRT calls; the service thread keeps the multithreaded apartment alive.
struct Apartment {
    Apartment() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~Apartment() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    HRESULT hr;
};

std::wstring xml_escape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        switch (c) {
            case L'&': out += L"&amp;"; break;
            case L'<': out += L"&lt;"; break;
            case L'>': out += L"&gt;"; break;
            case L'"': out += L"&quot;"; break;
            case L'\'': out += L"&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::wstring clock_time(std::chrono::system_clock::time_point t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_s(&tm, &tt);
    wchar_t buf[16];
    wcsftime(buf, 16, L"%H:%M:%S", &tm);
    return buf;
}

std::wstring headline(const Request& r) { return wide(r.client) + L" wants to change SAP"; }

std::wstring what(const Request& r) {
    std::wstring s = wide(r.summary);
    return r.sap.transaction.empty() ? s : wide(r.sap.transaction) + L" – " + s;
}

}  // namespace

std::wstring toast_xml(const Request& r) {
    auto arg = [&](Answer a) { return xml_escape(wide(answer_text(a, r))); };
    return L"<toast scenario=\"reminder\" activationType=\"foreground\" launch=\"" + arg(Answer::Details) + L"\">"
           L"<visual><binding template=\"ToastGeneric\">"
           L"<text>" + xml_escape(headline(r)) + L"</text>"
           L"<text>" + xml_escape(what(r)) + L"</text>"
           L"<text placement=\"attribution\">for " + xml_escape(wide(r.user)) + L" · denied at " + clock_time(r.expires_at) +
           L" without an answer</text>"
           L"</binding></visual><actions>"
           L"<action content=\"Approve\" activationType=\"foreground\" arguments=\"" + arg(Answer::Approve) + L"\"/>"
           L"<action content=\"Deny\" activationType=\"foreground\" arguments=\"" + arg(Answer::Deny) + L"\"/>"
           L"<action content=\"Details\" activationType=\"foreground\" arguments=\"" + arg(Answer::Details) + L"\"/>"
           L"</actions></toast>";
}

namespace {

// ---- the approval window ----------------------------------------------------------------------------------------

constexpr int kApprove = 100;
constexpr int kDeny = 101;
constexpr UINT kProgressSteps = 1000;

struct WindowState {
    const Request* r = nullptr;
    const std::function<bool()>* stop = nullptr;
    bool stopped = false;
    long long shown_seconds = -1;
    std::chrono::steady_clock::time_point opened;
    HICON icon_small = nullptr;
};

std::wstring countdown(long long s) { return L"Denied automatically in " + std::to_wstring(s) + L" s if you do not answer."; }

HRESULT CALLBACK window_events(HWND hwnd, UINT msg, WPARAM wp, LPARAM, LONG_PTR data) {
    auto* st = reinterpret_cast<WindowState*>(data);
    switch (msg) {
        case TDN_CREATED: {
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetForegroundWindow(hwnd);
            if (st->icon_small) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, LPARAM(st->icon_small));
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, kProgressSteps));
            SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, kProgressSteps, 0);
            break;
        }
        case TDN_TIMER: {
            if ((*st->stop)()) {
                st->stopped = true;
                SendMessageW(hwnd, TDM_CLICK_BUTTON, IDCANCEL, 0);
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            const auto total = std::chrono::duration<double>(st->r->deadline - st->opened).count();
            const auto left = std::chrono::duration<double>(st->r->deadline - now).count();
            const long long secs = left <= 0 ? 0 : (long long)(left + 0.999);
            if (secs != st->shown_seconds) {
                st->shown_seconds = secs;
                const std::wstring text = countdown(secs);
                SendMessageW(hwnd, TDM_UPDATE_ELEMENT_TEXT, TDE_FOOTER, LPARAM(text.c_str()));
            }
            if (total > 0) SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_POS, WPARAM(kProgressSteps * std::max(0.0, left) / total), 0);
            break;
        }
        default: break;
    }
    (void)wp;
    return S_OK;
}

}  // namespace

std::wstring window_text(const Request& r) {
    std::wstring s = L"Assistant: " + wide(r.client) + L"\nActing for: " + wide(r.user) + L"\n";
    const auto& sap = r.sap;
    if (sap.system.empty() && sap.transaction.empty()) {
        s += L"SAP: the session fairyfly works in\n";
    } else {
        s += L"SAP: system " + (sap.system.empty() ? L"?" : wide(sap.system));
        if (!sap.client.empty()) s += L", client " + wide(sap.client);
        if (!sap.transaction.empty()) s += L", transaction " + wide(sap.transaction);
        if (!sap.user.empty()) s += L", user " + wide(sap.user);
        s += L"\n";
    }
    s += L"\nWhat it will do:\n";
    for (size_t i = 0; i < r.steps.size(); i++) s += std::to_wstring(i + 1) + L". " + wide(r.steps[i]) + L"\n";
    return s;
}

namespace {

using TaskDialogIndirectFn = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);

TaskDialogIndirectFn task_dialog() {
    static TaskDialogIndirectFn fn = [] {
        HMODULE m = LoadLibraryW(L"comctl32.dll");
        return m ? reinterpret_cast<TaskDialogIndirectFn>(GetProcAddress(m, "TaskDialogIndirect")) : nullptr;
    }();
    return fn;
}

class Win32Ui : public Ui {
public:
    explicit Win32Ui(std::function<void(const std::string&)> on_answer) : on_answer_(std::move(on_answer)) {}

    bool notify(const Request& r) override {
        if (!ToastService::get().ready()) return false;
        Apartment apartment;
        auto failed = [&](const char* step, HRESULT hr) {
            spdlog::warn("approval {}: no notification ({} 0x{:08X}); the approval window opens", r.id, step, unsigned(hr));
            return false;
        };
        ComPtr<wn::IToastNotificationManagerStatics> manager;
        HRESULT hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(),
                                            IID_PPV_ARGS(&manager));
        if (FAILED(hr)) return failed("manager", hr);
        ComPtr<wn::IToastNotifier> notifier;
        if (FAILED(hr = manager->CreateToastNotifierWithId(HStringReference(kAppUserModelId).Get(), &notifier))) return failed("notifier", hr);
        wn::NotificationSetting setting = wn::NotificationSetting_Enabled;
        if (SUCCEEDED(notifier->get_Setting(&setting)) && setting != wn::NotificationSetting_Enabled) {
            spdlog::info("approval {}: notifications are off for fairyfly; the approval window opens", r.id);
            return false;
        }
        ComPtr<IInspectable> inspectable;
        if (FAILED(hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_Data_Xml_Dom_XmlDocument).Get(), &inspectable)))
            return failed("xml", hr);
        ComPtr<xml::IXmlDocument> doc;
        ComPtr<xml::IXmlDocumentIO> io;
        const std::wstring text = toast_xml(r);
        if (FAILED(hr = inspectable.As(&doc)) || FAILED(hr = doc.As(&io)) || FAILED(hr = io->LoadXml(HStringReference(text.c_str()).Get())))
            return failed("xml", hr);
        ComPtr<wn::IToastNotificationFactory> factory;
        ComPtr<wn::IToastNotification> toast;
        if (FAILED(hr = RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotification).Get(), IID_PPV_ARGS(&factory))) ||
            FAILED(hr = factory->CreateToastNotification(doc.Get(), &toast)))
            return failed("toast", hr);
        const std::wstring tag = std::to_wstring(r.id);
        ComPtr<wn::IToastNotification2> toast2;
        if (SUCCEEDED(toast.As(&toast2))) {
            toast2->put_Tag(HStringReference(tag.c_str()).Get());
            toast2->put_Group(HStringReference(kGroup).Get());
        }
        // gone from the notification centre when the question expires
        ComPtr<wf::IPropertyValueStatics> values;
        if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Foundation_PropertyValue).Get(), IID_PPV_ARGS(&values)))) {
            // DateTime: 100 ns ticks since 1601-01-01
            using ticks = std::chrono::duration<long long, std::ratio<1, 10000000>>;
            const auto since_1970 = std::chrono::duration_cast<ticks>(r.expires_at.time_since_epoch());
            wf::DateTime when{since_1970.count() + 116444736000000000LL};
            ComPtr<IInspectable> boxed;
            ComPtr<wf::IReference<wf::DateTime>> expiry;
            if (SUCCEEDED(values->CreateDateTime(when, &boxed)) && SUCCEEDED(boxed.As(&expiry))) toast->put_ExpirationTime(expiry.Get());
        }
        EventRegistrationToken token{};
        auto sink = on_answer_;
        toast->add_Activated(Microsoft::WRL::Callback<wf::ITypedEventHandler<wn::ToastNotification*, IInspectable*>>(
                                 [sink](wn::IToastNotification*, IInspectable* args) -> HRESULT {
                                     ComPtr<wn::IToastActivatedEventArgs> activated;
                                     if (args && SUCCEEDED(args->QueryInterface(IID_PPV_ARGS(&activated)))) {
                                         HString a;
                                         if (SUCCEEDED(activated->get_Arguments(a.GetAddressOf())))
                                             sink(com::wide_to_utf8(WindowsGetStringRawBuffer(a.Get(), nullptr)));
                                     }
                                     return S_OK;
                                 })
                                 .Get(),
                             &token);
        if (FAILED(hr = notifier->Show(toast.Get()))) return failed("show", hr);
        std::lock_guard<std::mutex> l(mu_);
        shown_[r.id] = Shown{notifier, toast};
        return true;
    }

    void withdraw(const Request& r) override {
        Apartment apartment;
        Shown s;
        {
            std::lock_guard<std::mutex> l(mu_);
            auto i = shown_.find(r.id);
            if (i != shown_.end()) {
                s = i->second;
                shown_.erase(i);
            }
        }
        if (s.notifier && s.toast) s.notifier->Hide(s.toast.Get());
        ComPtr<wn::IToastNotificationManagerStatics> manager;
        ComPtr<wn::IToastNotificationManagerStatics2> manager2;
        ComPtr<wn::IToastNotificationHistory> history;
        const std::wstring tag = std::to_wstring(r.id);
        if (SUCCEEDED(RoGetActivationFactory(HStringReference(RuntimeClass_Windows_UI_Notifications_ToastNotificationManager).Get(), IID_PPV_ARGS(&manager))) &&
            SUCCEEDED(manager.As(&manager2)) && SUCCEEDED(manager2->get_History(&history)))
            history->RemoveGroupedTagWithId(HStringReference(tag.c_str()).Get(), HStringReference(kGroup).Get(), HStringReference(kAppUserModelId).Get());
    }

    Answer ask(const Request& r, const std::function<bool()>& stop) override {
        auto fn = task_dialog();
        if (!fn) {
            spdlog::error("approval {}: denied, the approval window cannot be shown (common controls 6 missing)", r.id);
            return Answer::Deny;
        }
        WindowState st;
        st.r = &r;
        st.stop = &stop;
        st.opened = std::chrono::steady_clock::now();
        const std::wstring title = L"fairyfly – approve a change in SAP";
        const std::wstring head = headline(r);
        const std::wstring body = window_text(r);
        const int minutes = std::max(1, r.window_s / 60);
        const std::wstring approve = L"Approve\nAllow changes in SAP for the next " + std::to_wstring(minutes) +
                                     (minutes == 1 ? L" minute" : L" minutes") + L" without asking again";
        const std::wstring deny = L"Deny\nNothing is changed in SAP";
        const auto left = std::chrono::duration_cast<std::chrono::seconds>(r.deadline - st.opened).count();
        const std::wstring footer = countdown(std::max<long long>(0, left));
        TASKDIALOG_BUTTON buttons[] = {{kApprove, approve.c_str()}, {kDeny, deny.c_str()}};

        TASKDIALOGCONFIG c{};
        c.cbSize = sizeof(c);
        c.hInstance = GetModuleHandleW(nullptr);
        c.dwFlags = TDF_USE_COMMAND_LINKS | TDF_ALLOW_DIALOG_CANCELLATION | TDF_CALLBACK_TIMER | TDF_SHOW_PROGRESS_BAR | TDF_SIZE_TO_CONTENT;
        c.pszWindowTitle = title.c_str();
        // the link tray's teal mark
        HICON big = static_cast<HICON>(LoadImageW(c.hInstance, MAKEINTRESOURCEW(IDI_LINK_RUNNING), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                                  GetSystemMetrics(SM_CYICON), 0));
        st.icon_small = static_cast<HICON>(LoadImageW(c.hInstance, MAKEINTRESOURCEW(IDI_LINK_RUNNING), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                      GetSystemMetrics(SM_CYSMICON), 0));
        if (big) {
            c.dwFlags |= TDF_USE_HICON_MAIN;
            c.hMainIcon = big;
        } else {
            c.pszMainIcon = TD_WARNING_ICON;
        }
        c.pszMainInstruction = head.c_str();
        c.pszContent = body.c_str();
        c.cButtons = UINT(std::size(buttons));
        c.pButtons = buttons;
        c.nDefaultButton = kDeny;  // Enter and Esc deny
        c.pszFooter = footer.c_str();
        c.pszFooterIcon = TD_INFORMATION_ICON;
        c.pfCallback = window_events;
        c.lpCallbackData = reinterpret_cast<LONG_PTR>(&st);
        int pressed = 0;
        const HRESULT hr = fn(&c, &pressed, nullptr, nullptr);
        if (big) DestroyIcon(big);
        if (st.icon_small) DestroyIcon(st.icon_small);
        if (FAILED(hr)) {
            spdlog::error("approval {}: denied, the approval window failed (0x{:08X})", r.id, unsigned(hr));
            return Answer::Deny;
        }
        if (st.stopped) return Answer::None;
        return pressed == kApprove ? Answer::Approve : Answer::Deny;
    }

private:
    struct Shown {
        ComPtr<wn::IToastNotifier> notifier;
        ComPtr<wn::IToastNotification> toast;
    };
    std::function<void(const std::string&)> on_answer_;
    std::mutex mu_;
    std::map<uint64_t, Shown> shown_;
};

}  // namespace

bool desktop_can_ask() {
    USEROBJECTFLAGS flags{};
    HWINSTA station = GetProcessWindowStation();
    if (!station || !GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), nullptr) || !(flags.dwFlags & WSF_VISIBLE))
        return false;  // a service or another non-interactive window station
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSSessionInfoEx, &buffer, &bytes) || !buffer) return false;
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
    // active (not a disconnected remote session) and not locked (Windows 8 and later: WTS_SESSIONSTATE_LOCK = locked)
    const bool ok = info->Level == 1 && info->Data.WTSInfoExLevel1.SessionState == WTSActive &&
                    info->Data.WTSInfoExLevel1.SessionFlags == WTS_SESSIONSTATE_UNLOCK;
    WTSFreeMemory(buffer);
    return ok;
}

std::shared_ptr<Ui> make_win32_ui(std::function<void(const std::string&)> on_answer) { return std::make_shared<Win32Ui>(std::move(on_answer)); }

std::shared_ptr<ffy::link::Approver> make_desktop_approver(std::function<SapContext()> sap, Options options) {
    // the broker and the notification's answers know each other only through this weak link
    auto link = std::make_shared<std::weak_ptr<Broker>>();
    auto on_answer = [link](const std::string& text) {
        if (auto b = link->lock()) b->answer_text(text);
    };
    {
        std::lock_guard<std::mutex> l(g_sink_mu);
        g_sink = on_answer;  // the COM activator (the notification centre)
    }
    Desk desk;
    desk.can_ask = desktop_can_ask;
    desk.sap = std::move(sap);
    auto broker = std::make_shared<Broker>(make_win32_ui(on_answer), std::move(desk), options);
    *link = broker;
    return make_approver(broker);
}

}  // namespace fairyfly::link::approval
