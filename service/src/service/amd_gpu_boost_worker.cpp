#include "amd_gpu_boost_worker.hpp"
#include "amd_gpu_pci_address.hpp"

#include <windows.h>

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace llavon::service {
namespace {

using Result = GpuActivityLease::Result;
constexpr std::string_view worker_switch = "--amd-gpu-boost-worker";
constexpr DWORD startup_timeout_ms = 15000;
constexpr DWORD command_timeout_ms = 1000;

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { if (handle) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;
Handle checked_handle(HANDLE raw) {
    if (!raw || raw == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("AMD boost IPC handle creation failed: " + std::to_string(GetLastError()));
    }
    return Handle(raw);
}

struct SharedState {
    // Win32 interlocked operations provide cross-process ordering. std::atomic
    // is intentionally not used: its interprocess guarantees are not portable.
    volatile LONG enabled;
    volatile LONG result;
};
struct UnmapViewDeleter {
    void operator()(SharedState* view) const noexcept { if (view) UnmapViewOfFile(view); }
};
using View = std::unique_ptr<SharedState, UnmapViewDeleter>;
View map_view(HANDLE mapping) {
    View view(static_cast<SharedState*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedState))));
    if (!view) throw std::runtime_error("AMD boost IPC mapping failed");
    return view;
}

class InheritedHandles final {
public:
    explicit InheritedHandles(const std::array<HANDLE, 5>& handles) {
        for (std::size_t i = 0; i < handles.size(); ++i) {
            HANDLE duplicate = nullptr;
            if (!DuplicateHandle(GetCurrentProcess(), handles[i], GetCurrentProcess(), &duplicate,
                                 0, TRUE, DUPLICATE_SAME_ACCESS)) {
                throw std::runtime_error("AMD boost handle inheritance failed");
            }
            owned_[i] = checked_handle(duplicate);
            raw_[i] = duplicate;
        }
        SIZE_T size = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        attributes_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(attributes_, 1, 0, &size)) {
            attributes_ = nullptr;
            throw std::runtime_error("AMD boost process attributes failed");
        }
        if (!UpdateProcThreadAttribute(attributes_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                       raw_.data(), sizeof(raw_), nullptr, nullptr)) {
            DeleteProcThreadAttributeList(attributes_);
            attributes_ = nullptr;
            throw std::runtime_error("AMD boost restricted handle list failed");
        }
    }
    ~InheritedHandles() { if (attributes_) DeleteProcThreadAttributeList(attributes_); }
    InheritedHandles(const InheritedHandles&) = delete;
    InheritedHandles& operator=(const InheritedHandles&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes() const noexcept { return attributes_; }
    std::wstring arguments() const {
        std::wstring result;
        for (const auto handle : raw_) result += L" " + std::to_wstring(reinterpret_cast<std::uintptr_t>(handle));
        return result;
    }
private:
    std::array<Handle, 5> owned_;
    std::array<HANDLE, 5> raw_{};
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes_ = nullptr;
};

class WorkerClient final {
public:
    explicit WorkerClient(std::string_view device_id)
        : request_(checked_handle(CreateEventW(nullptr, FALSE, FALSE, nullptr))),
          stop_(checked_handle(CreateEventW(nullptr, TRUE, FALSE, nullptr))),
          reply_(checked_handle(CreateEventW(nullptr, FALSE, FALSE, nullptr))),
          mapping_(checked_handle(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                                     0, sizeof(SharedState), nullptr))),
          view_(map_view(mapping_.get())) {
        if (!amd_pci_address(device_id)) throw std::runtime_error("invalid AMD PCI address");
        const auto parent = checked_handle(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                                       FALSE, GetCurrentProcessId()));
        InheritedHandles inherited({parent.get(), request_.get(), stop_.get(), reply_.get(), mapping_.get()});
        std::wstring executable(32768, L'\0');
        const DWORD size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!size || size >= executable.size()) throw std::runtime_error("AMD boost executable path unavailable");
        executable.resize(size);
        std::wstring command = L"\"" + executable + L"\" --amd-gpu-boost-worker " +
            std::wstring(device_id.begin(), device_id.end()) + inherited.arguments();
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.lpAttributeList = inherited.attributes();
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                            &startup.StartupInfo, &process)) {
            throw std::runtime_error("AMD boost worker launch failed: " + std::to_string(GetLastError()));
        }
        process_.reset(process.hProcess);
        const Handle thread(process.hThread);
        if (wait_reply(startup_timeout_ms) != Result::success) {
            (void)SetEvent(stop_.get());
            throw std::runtime_error("AMD driver clock-mode worker is unavailable or busy");
        }
    }

    ~WorkerClient() {
        // Do not terminate the child: it owns restoration, including its retries.
        (void)SetEvent(stop_.get());
        (void)WaitForSingleObject(process_.get(), command_timeout_ms);
    }
    WorkerClient(const WorkerClient&) = delete;
    WorkerClient& operator=(const WorkerClient&) = delete;

    Result set(bool enabled) noexcept {
        if (failed_) return Result::unavailable;
        InterlockedExchange(&view_->enabled, enabled ? 1 : 0);
        if (!SetEvent(request_.get())) return disconnect();
        const auto result = wait_reply(command_timeout_ms);
        if (failed_) return disconnect();
        if (result == Result::success) {
            auto& logged = enabled ? logged_enable_ : logged_release_;
            if (!logged) {
                std::clog << "[SRV] AMD GPU boost " << (enabled ? "applied: mode=Peak" : "restored: mode=Default") << '\n';
                logged = true;
            }
        } else {
            std::clog << "[WARN] AMD GPU clock-mode request failed: enabled=" << enabled
                      << " result=" << static_cast<int>(result) << '\n';
        }
        return result;
    }

private:
    Result disconnect() noexcept {
        failed_ = true;
        (void)SetEvent(stop_.get());
        std::clog << "[WARN] AMD boost worker disconnected; further boost requests suspended\n";
        return Result::unavailable;
    }
    Result wait_reply(DWORD timeout) noexcept {
        const std::array handles{process_.get(), reply_.get()};
        if (WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout) !=
            WAIT_OBJECT_0 + 1) {
            failed_ = true;
            return Result::unavailable;
        }
        return static_cast<Result>(InterlockedCompareExchange(&view_->result, 0, 0));
    }
    Handle request_;
    Handle stop_;
    Handle reply_;
    Handle mapping_;
    View view_;
    Handle process_;
    bool failed_ = false;
    bool logged_enable_ = false;
    bool logged_release_ = false;
};

Result invoke(GpuActivityLease::Boost& driver, bool enabled) noexcept {
    try { return driver(enabled); }
    catch (...) { return Result::retry_later; }
}

int worker_main(int argc, char** argv, AmdBoostDriverFactory factory) {
    if (argc != 8 || !amd_pci_address(argv[2])) return 2;
    std::array<Handle, 5> handles;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        const std::string_view text(argv[i + 3]);
        std::uintptr_t value = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
        if (error != std::errc{} || end != text.data() + text.size()) return 2;
        DWORD flags = 0;
        const auto handle = reinterpret_cast<HANDLE>(value);
        if (!value || !GetHandleInformation(handle, &flags)) return 2;
        handles[i].reset(handle);
        (void)SetHandleInformation(handle, HANDLE_FLAG_INHERIT, 0);
    }
    const auto parent = handles[0].get();
    const auto request = handles[1].get();
    const auto stop = handles[2].get();
    const auto reply = handles[3].get();
    if (!GetProcessId(parent) || GetProcessId(parent) == GetCurrentProcessId()) return 2;
    auto view = map_view(handles[4].get());
    const auto respond = [&](Result result) {
        InterlockedExchange(&view->result, static_cast<LONG>(result));
        return SetEvent(reply) != FALSE;
    };

    // Normalize the BDF so two differently formatted IDs cannot obtain separate
    // locks. Keep ownership until Default succeeds or restoration retries end.
    const auto address = *amd_pci_address(argv[2]);
    const auto name = L"Local\\LlavonAmdClockMode-" + std::to_wstring(address.bus) + L"-" +
        std::to_wstring(address.device) + L"-" + std::to_wstring(address.function);
    const auto mutex = checked_handle(CreateMutexW(nullptr, FALSE, name.c_str()));
    const auto lock = WaitForSingleObject(mutex.get(), 0);
    if (lock != WAIT_OBJECT_0 && lock != WAIT_ABANDONED) {
        (void)respond(Result::unavailable);
        return 3;
    }
    struct Unlock {
        HANDLE mutex;
        ~Unlock() { ReleaseMutex(mutex); }
    } unlock{mutex.get()};

    GpuActivityLease::Boost driver;
    try { driver = factory(argv[2]); } catch (...) {}
    if (!driver) {
        (void)respond(Result::unavailable);
        return 3;
    }
    bool pending = false;
    bool running = respond(Result::success);
    const std::array waits{parent, stop, request};
    while (running) {
        const auto signal = WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, INFINITE);
        if (signal != WAIT_OBJECT_0 + 2) break;
        const bool enabled = InterlockedCompareExchange(&view->enabled, 0, 0) != 0;
        // Record restoration ownership before entering any driver call.
        if (enabled) pending = true;
        auto result = invoke(driver, enabled);
        if (!enabled && result == Result::success) pending = false;
        if (enabled && result != Result::success) {
            if (invoke(driver, false) == Result::success) pending = false;
            else {
                result = Result::unavailable;
                running = false;
            }
        }
        if (!respond(result)) break;
    }
    // Covers normal disposal, service termination, and an IPC timeout. Retries
    // run here independently of the service's inference executor and lifetime.
    for (unsigned attempt = 0; pending && attempt < 30; ++attempt) {
        if (invoke(driver, false) == Result::success) pending = false;
        else Sleep(1000);
    }
    return pending ? 4 : 0;
}

} // namespace

GpuActivityLease::Boost make_amd_boost_worker(std::string_view device_id) {
    auto worker = std::make_unique<WorkerClient>(device_id);
    return [worker = std::move(worker)](bool enabled) { return worker->set(enabled); };
}

std::optional<int> run_amd_boost_worker(int argc, char** argv, AmdBoostDriverFactory factory) noexcept {
    if (argc < 2 || std::string_view(argv[1]) != worker_switch) return std::nullopt;
    try { return worker_main(argc, argv, factory); }
    catch (...) { return 3; }
}

} // namespace llavon::service
