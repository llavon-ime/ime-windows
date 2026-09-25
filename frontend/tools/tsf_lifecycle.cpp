#include <msctf.h>
#include <msi.h>
#include <objbase.h>
#include <sddl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "system/globals.h"
#include "system/serviceLauncher.hpp"

namespace {

constexpr wchar_t service_window_class[] = L"LlavonImeServiceTrayWindow";
constexpr wchar_t service_shutdown_message[] = L"LlavonIme.SafeShutdownV2";
constexpr LRESULT shutdown_acknowledged = 0x4c4c4156;
constexpr DWORD graceful_shutdown_timeout_ms = 30000;
constexpr DWORD forced_shutdown_timeout_ms = 5000;
constexpr DWORD update_guard_ready_timeout_ms = 30000;
constexpr DWORD update_guard_lifetime_ms = 15 * 60 * 1000;
constexpr wchar_t update_guard_ready_event[] = L"Local\\LlavonImeBackendUpdateReady";
constexpr wchar_t update_guard_done_event[] = L"Local\\LlavonImeBackendUpdateDone";

class ComApartment {
public:
    ComApartment() noexcept
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE)) {}

    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    HRESULT result() const noexcept { return result_; }

private:
    HRESULT result_;
};

class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    void reset() noexcept {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = nullptr;
    }

    HANDLE handle_ = nullptr;
};

class ScopedLaunchMutex {
public:
    ScopedLaunchMutex() noexcept {
        handle_ = UniqueHandle(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                          tsf::service_launch_mutex_name));
        if (!handle_) {
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    L"D:(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1,
                    &descriptor, nullptr)) {
                return;
            }
            SECURITY_ATTRIBUTES security{sizeof(security), descriptor, FALSE};
            handle_ = UniqueHandle(CreateMutexW(&security, FALSE,
                                                tsf::service_launch_mutex_name));
            LocalFree(descriptor);
        }
        if (handle_) {
            const DWORD result = WaitForSingleObject(handle_.get(), 30000);
            owns_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }

    ~ScopedLaunchMutex() {
        if (owns_) ReleaseMutex(handle_.get());
    }

    ScopedLaunchMutex(const ScopedLaunchMutex&) = delete;
    ScopedLaunchMutex& operator=(const ScopedLaunchMutex&) = delete;

    bool owns() const noexcept { return owns_; }

private:
    UniqueHandle handle_;
    bool owns_ = false;
};

int hold_update_guard() noexcept {
    ScopedLaunchMutex launch_mutex;
    if (!launch_mutex.owns()) return ERROR_TIMEOUT;

    UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, update_guard_ready_event));
    UniqueHandle done(OpenEventW(SYNCHRONIZE, FALSE, update_guard_done_event));
    if (!ready || !done || !SetEvent(ready.get())) return ERROR_FUNCTION_FAILED;

    const DWORD result = WaitForSingleObject(done.get(), update_guard_lifetime_ms);
    return result == WAIT_OBJECT_0 || result == WAIT_TIMEOUT
               ? ERROR_SUCCESS
               : ERROR_FUNCTION_FAILED;
}

void end_update_guard() noexcept {
    UniqueHandle done(OpenEventW(EVENT_MODIFY_STATE, FALSE, update_guard_done_event));
    if (done) SetEvent(done.get());
}

HRESULT begin_update_guard(std::wstring_view service_path) {
    UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, update_guard_ready_event));
    UniqueHandle done(CreateEventW(nullptr, TRUE, FALSE, update_guard_done_event));
    if (!ready || !done || !ResetEvent(ready.get()) || !ResetEvent(done.get())) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    std::wstring executable(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (copied == 0) return HRESULT_FROM_WIN32(GetLastError());
        if (copied < executable.size() - 1) {
            executable.resize(copied);
            break;
        }
        executable.resize(executable.size() * 2);
    }

    std::wstring command = L"\"" + executable + L"\" guard-update \"" +
                           std::wstring(service_path) + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    UniqueHandle child(process.hProcess);
    UniqueHandle thread(process.hThread);

    const DWORD result = WaitForSingleObject(ready.get(), update_guard_ready_timeout_ms);
    if (result != WAIT_OBJECT_0) {
        SetEvent(done.get());
        return result == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                                      : HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
}

struct ServiceProcess {
    DWORD id;
    UniqueHandle handle;
};

int exit_code_from_hresult(HRESULT result) noexcept {
    if (SUCCEEDED(result)) {
        return ERROR_SUCCESS;
    }

    const DWORD code = HRESULT_FACILITY(result) == FACILITY_WIN32
                           ? HRESULT_CODE(result)
                           : ERROR_FUNCTION_FAILED;
    return static_cast<int>(code == ERROR_SUCCESS ? ERROR_FUNCTION_FAILED : code);
}

bool equal_path(std::wstring_view left, std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

std::wstring absolute_path(std::wstring_view path) {
    const std::wstring path_string(path);
    const DWORD required = GetFullPathNameW(path_string.c_str(), 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }

    std::wstring result(required, L'\0');
    const DWORD copied = GetFullPathNameW(path_string.c_str(), required, result.data(), nullptr);
    if (copied == 0 || copied >= required) {
        return {};
    }
    result.resize(copied);
    return result;
}

HRESULT find_service_processes(std::wstring_view expected_path,
                               std::vector<ServiceProcess>& processes) {
    DWORD current_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    const std::wstring normalized_path = absolute_path(expected_path);
    if (normalized_path.empty()) {
        return HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
    }
    const std::wstring expected_name =
        normalized_path.substr(normalized_path.find_last_of(L"\\/") + 1);

    UniqueHandle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (!snapshot) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (!Process32FirstW(snapshot.get(), &entry)) {
        const DWORD error = GetLastError();
        return error == ERROR_NO_MORE_FILES ? S_OK : HRESULT_FROM_WIN32(error);
    }

    do {
        if (_wcsicmp(entry.szExeFile, expected_name.c_str()) != 0) {
            continue;
        }

        DWORD process_session = 0;
        if (!ProcessIdToSessionId(entry.th32ProcessID, &process_session) ||
            process_session != current_session) {
            continue;
        }

        UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE |
                                             PROCESS_TERMINATE,
                                         FALSE, entry.th32ProcessID));
        if (!process) {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        std::wstring process_path(32768, L'\0');
        DWORD path_length = static_cast<DWORD>(process_path.size());
        if (!QueryFullProcessImageNameW(process.get(), 0, process_path.data(), &path_length)) {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        process_path.resize(path_length);
        if (equal_path(process_path, normalized_path)) {
            processes.push_back(ServiceProcess{entry.th32ProcessID, std::move(process)});
        }
    } while (Process32NextW(snapshot.get(), &entry));

    const DWORD error = GetLastError();
    return error == ERROR_NO_MORE_FILES ? S_OK : HRESULT_FROM_WIN32(error);
}

HRESULT installed_service_path(std::wstring_view product_codes,
                               std::wstring& service_path) {
    while (!product_codes.empty()) {
        const auto separator = product_codes.find(L';');
        const std::wstring product_code(product_codes.substr(0, separator));
        DWORD length = 0;
        const UINT result = MsiGetProductInfoW(
            product_code.c_str(), INSTALLPROPERTY_INSTALLLOCATION, nullptr, &length);
        if ((result == ERROR_SUCCESS || result == ERROR_MORE_DATA) && length > 0) {
            std::wstring location(length + 1, L'\0');
            DWORD capacity = static_cast<DWORD>(location.size());
            const UINT read_result = MsiGetProductInfoW(
                product_code.c_str(), INSTALLPROPERTY_INSTALLLOCATION,
                location.data(), &capacity);
            if (read_result != ERROR_SUCCESS) {
                return HRESULT_FROM_WIN32(read_result);
            }
            location.resize(capacity);
            service_path = (std::filesystem::path(location) / L"bin" /
                            L"llavon-ime-service.exe").wstring();
            return S_OK;
        }
        if (separator == std::wstring_view::npos) break;
        product_codes.remove_prefix(separator + 1);
    }
    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
}

HRESULT stop_service(std::wstring_view service_path) noexcept {
    try {
        std::vector<ServiceProcess> processes;
        HRESULT result = find_service_processes(service_path, processes);
        if (FAILED(result) || processes.empty()) {
            return result;
        }

        DWORD gracefully_stopping_process_id = 0;
        const HWND service_window = FindWindowW(service_window_class, nullptr);
        if (service_window) {
            DWORD window_process_id = 0;
            GetWindowThreadProcessId(service_window, &window_process_id);
            for (const auto& process : processes) {
                if (process.id == window_process_id) {
                    const UINT message = RegisterWindowMessageW(service_shutdown_message);
                    if (message != 0) {
                        DWORD_PTR ignored = 0;
                        if (SendMessageTimeoutW(service_window, message, 0, 0,
                                                SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000,
                                                &ignored) != 0 &&
                            ignored == static_cast<DWORD_PTR>(shutdown_acknowledged)) {
                            gracefully_stopping_process_id = process.id;
                        }
                    }
                    break;
                }
            }
        }

        for (auto& process : processes) {
            const DWORD graceful_wait = process.id == gracefully_stopping_process_id
                                            ? graceful_shutdown_timeout_ms
                                            : 0;
            DWORD wait_result = WaitForSingleObject(process.handle.get(), graceful_wait);
            if (wait_result == WAIT_OBJECT_0) {
                continue;
            }
            if (wait_result != WAIT_TIMEOUT) {
                return HRESULT_FROM_WIN32(GetLastError());
            }

            // Older installed versions do not understand SafeShutdownV2 and
            // can fault in Windows.UI.Xaml while closing their settings UI.
            // End those processes without invoking their broken UI teardown.
            if (!TerminateProcess(process.handle.get(), ERROR_PROCESS_ABORTED)) {
                const DWORD terminate_error = GetLastError();
                if (WaitForSingleObject(process.handle.get(), 0) == WAIT_OBJECT_0) {
                    continue;
                }
                return HRESULT_FROM_WIN32(terminate_error);
            }
            wait_result = WaitForSingleObject(process.handle.get(), forced_shutdown_timeout_ms);
            if (wait_result != WAIT_OBJECT_0) {
                return wait_result == WAIT_TIMEOUT
                           ? HRESULT_FROM_WIN32(ERROR_TIMEOUT)
                           : HRESULT_FROM_WIN32(GetLastError());
            }
        }
        return S_OK;
    } catch (...) {
        return E_FAIL;
    }
}

HRESULT release_text_service() noexcept {
    ITfInputProcessorProfileMgr* profile_manager = nullptr;
    HRESULT result = CoCreateInstance(
        CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITfInputProcessorProfileMgr, reinterpret_cast<void**>(&profile_manager));
    if (FAILED(result)) {
        return result;
    }

    // Deactivation asks every TSF thread on the current desktop to run the
    // text service's Deactivate path before Windows Installer replaces files.
    // ReleaseInputProcessor then drops TSF's remaining object references and
    // requests COM to unload DLLs whose DllCanUnloadNow returns S_OK.
    (void)profile_manager->DeactivateProfile(
        TF_PROFILETYPE_INPUTPROCESSOR, tsf::Globals::tsf_language_id,
        tsf::Globals::text_service_clsid, tsf::Globals::text_service_profile_guid,
        nullptr, TF_IPPMF_FORSESSION);
    result = profile_manager->ReleaseInputProcessor(
        tsf::Globals::text_service_clsid, TF_RIP_FLAG_FREEUNUSEDLIBRARIES);

    profile_manager->Release();
    CoFreeUnusedLibrariesEx(0, 0);
    return result;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments || argument_count != 3 ||
        (std::wstring_view(arguments[1]) != L"prepare-update" &&
         std::wstring_view(arguments[1]) != L"prepare-update-product" &&
         std::wstring_view(arguments[1]) != L"prepare-uninstall" &&
         std::wstring_view(arguments[1]) != L"start-service" &&
         std::wstring_view(arguments[1]) != L"restart-service" &&
         std::wstring_view(arguments[1]) != L"end-update" &&
         std::wstring_view(arguments[1]) != L"guard-update")) {
        if (arguments) {
            LocalFree(arguments);
        }
        return ERROR_INVALID_PARAMETER;
    }

    const std::wstring operation = arguments[1];
    std::wstring service_path = arguments[2];
    LocalFree(arguments);

    if (operation == L"prepare-update-product") {
        std::wstring installed_path;
        const HRESULT result = installed_service_path(service_path, installed_path);
        if (FAILED(result)) return exit_code_from_hresult(result);
        service_path = std::move(installed_path);
    }

    if (operation == L"start-service") {
        return tsf::launch_process_with_shell_parent(service_path)
                   ? ERROR_SUCCESS
                   : ERROR_PROCESS_ABORTED;
    }

    if (operation == L"guard-update") return hold_update_guard();

    if (operation == L"end-update") {
        end_update_guard();
        return ERROR_SUCCESS;
    }

    const bool updating = operation == L"prepare-update" ||
                          operation == L"prepare-update-product";
    if (updating) {
        const HRESULT guard_result = begin_update_guard(service_path);
        if (FAILED(guard_result)) return exit_code_from_hresult(guard_result);
    }

    const HRESULT stop_result = stop_service(service_path);
    if (FAILED(stop_result)) {
        if (updating || operation == L"restart-service") end_update_guard();
        return exit_code_from_hresult(stop_result);
    }

    if (operation == L"restart-service") {
        // An input request can relaunch the old service while MSI is replacing
        // files. Stop that process before starting the newly installed binary.
        const bool launched = tsf::launch_process_with_shell_parent(service_path);
        end_update_guard();
        return launched ? ERROR_SUCCESS : ERROR_PROCESS_ABORTED;
    }

    ComApartment apartment;
    if (FAILED(apartment.result())) {
        if (updating) end_update_guard();
        return exit_code_from_hresult(apartment.result());
    }

    const HRESULT release_result = release_text_service();
    if (FAILED(release_result) && updating) end_update_guard();
    return exit_code_from_hresult(release_result);
}
