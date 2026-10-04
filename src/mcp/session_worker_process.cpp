#include "include/mcp/session_worker_process.h"

#include <array>
#include <atomic>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace fairyfly::mcp {

struct SessionWorkerProcess::Impl {
    explicit Impl(std::wstring path, std::wstring arg) : executable(std::move(path)), argument(std::move(arg)) {}
    std::wstring executable;
    std::wstring argument;
    std::mutex mutex;
    std::atomic<bool> failed{false};
    std::atomic<bool> terminated{false};
#ifdef _WIN32
    std::atomic<HANDLE> process{nullptr};
    HANDLE input = nullptr;   // broker writes to child's stdin
    HANDLE output = nullptr;  // broker reads from child's stdout
    std::string pending;
#endif
    ~Impl() {
#ifdef _WIN32
        if (input) CloseHandle(input);
        if (output) CloseHandle(output);
        if (HANDLE child = process.load()) {
            if (WaitForSingleObject(child, 0) == WAIT_TIMEOUT) {
                TerminateProcess(child, 1);
                WaitForSingleObject(child, 1000);
            }
            CloseHandle(child);
        }
#endif
    }
};

SessionWorkerProcess::SessionWorkerProcess(std::wstring executable, std::wstring argument)
    : impl_(std::make_unique<Impl>(std::move(executable), std::move(argument))) {}
SessionWorkerProcess::~SessionWorkerProcess() = default;

bool SessionWorkerProcess::healthy() const noexcept {
    return !impl_->failed.load(std::memory_order_acquire) &&
           !impl_->terminated.load(std::memory_order_acquire);
}

void SessionWorkerProcess::terminate() {
    impl_->terminated.store(true, std::memory_order_release);
#ifdef _WIN32
    if (HANDLE child = impl_->process.load(std::memory_order_acquire))
        TerminateProcess(child, 1);
#endif
}

#ifdef _WIN32
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE release() { HANDLE h = value; value = nullptr; return h; }
};

void start_child(SessionWorkerProcess::Impl& child) {
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle stdin_read, stdin_write, stdout_read, stdout_write;
    if (!CreatePipe(&stdin_read.value, &stdin_write.value, &sa, 0) ||
        !CreatePipe(&stdout_read.value, &stdout_write.value, &sa, 0) ||
        !SetHandleInformation(stdin_write.value, HANDLE_FLAG_INHERIT, 0) ||
        !SetHandleInformation(stdout_read.value, HANDLE_FLAG_INHERIT, 0))
        throw WorkerTransportError("WORKER_UNAVAILABLE", "cannot create private worker pipes");
    Handle stderr_handle(CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (stderr_handle.value == INVALID_HANDLE_VALUE)
        throw WorkerTransportError("WORKER_UNAVAILABLE", "cannot open worker diagnostic sink");

    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes))
        throw WorkerTransportError("WORKER_UNAVAILABLE", "cannot initialize worker handle list");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } guard{attributes};
    std::array<HANDLE, 3> inherited{stdin_read.value, stdout_write.value, stderr_handle.value};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(inherited), nullptr, nullptr))
        throw WorkerTransportError("WORKER_UNAVAILABLE", "cannot restrict worker inherited handles");

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_read.value;
    startup.StartupInfo.hStdOutput = stdout_write.value;
    startup.StartupInfo.hStdError = stderr_handle.value;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION pi{};
    std::wstring command = L"\"" + child.executable + L"\"";
    if (!child.argument.empty()) command += L" " + child.argument;
    if (!CreateProcessW(child.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, nullptr, nullptr,
                        &startup.StartupInfo, &pi))
        throw WorkerTransportError("WORKER_UNAVAILABLE", "cannot start session worker");
    CloseHandle(pi.hThread);
    child.process.store(pi.hProcess, std::memory_order_release);
    child.input = stdin_write.release();
    child.output = stdout_read.release();
    if (child.terminated.load(std::memory_order_acquire)) TerminateProcess(pi.hProcess, 1);
}

std::string read_response(SessionWorkerProcess::Impl& child) {
    constexpr std::size_t max_bytes = 16 * 1024 * 1024;
    while (true) {
        const auto end = child.pending.find('\n');
        if (end != std::string::npos) {
            if (end > max_bytes)
                throw WorkerTransportError("OUTCOME_UNKNOWN", "worker response exceeds limit");
            std::string line = child.pending.substr(0, end);
            child.pending.erase(0, end + 1);
            return line;
        }
        if (child.pending.size() > max_bytes)
            throw WorkerTransportError("OUTCOME_UNKNOWN", "worker response exceeds limit");
        std::array<char, 8192> buffer{};
        DWORD received = 0;
        if (!ReadFile(child.output, buffer.data(), static_cast<DWORD>(buffer.size()), &received, nullptr) || received == 0)
            throw WorkerTransportError("OUTCOME_UNKNOWN", "worker exited before returning a result");
        child.pending.append(buffer.data(), received);
    }
}
} // namespace
#endif

json SessionWorkerProcess::invoke(const WorkerCall& call, std::function<void()> before_send) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->failed || impl_->terminated.load())
        throw WorkerTransportError("WORKER_UNAVAILABLE", "session worker has failed");
#ifdef _WIN32
    if (!impl_->process.load()) start_child(*impl_);
    std::string frame = encode_worker_call(call) + '\n';
    if (frame.size() > kWorkerRequestMaxBytes) throw WorkerTransportError("WORKER_BAD_REQUEST", "worker request exceeds limit");
    if (before_send) before_send();
    std::size_t written_total = 0;
    while (written_total < frame.size()) {
        DWORD written = 0;
        if (!WriteFile(impl_->input, frame.data() + written_total,
                       static_cast<DWORD>(frame.size() - written_total), &written, nullptr) || written == 0) {
            impl_->failed = true;
            throw WorkerTransportError("OUTCOME_UNKNOWN", "session worker pipe closed during submission");
        }
        written_total += written;
    }
    try {
        const json response = json::parse(read_response(*impl_));
        if (!response.is_object() || !response.contains("version") || response["version"] != 1 ||
            !response.contains("id") || response["id"] != call.id ||
            !response.contains("result") || !response["result"].is_object() ||
            !response["result"].contains("status") || !response["result"]["status"].is_string())
            throw WorkerTransportError("OUTCOME_UNKNOWN", "invalid session worker response");
        if (response["result"]["status"] == "error" && response["result"].contains("error") &&
            response["result"]["error"].is_object() &&
            response["result"]["error"].value("code", "") == "OUTCOME_UNKNOWN")
            impl_->failed = true;
        return response["result"];
    } catch (...) {
        impl_->failed = true;
        throw WorkerTransportError("OUTCOME_UNKNOWN", "session worker failed after command submission");
    }
#else
    (void)call;
    (void)before_send;
    throw WorkerTransportError("WORKER_UNAVAILABLE", "session worker requires Windows");
#endif
}

} // namespace fairyfly::mcp
