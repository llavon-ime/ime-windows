#include <msctf.h>
#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <windows.h>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "system/globals.h"

namespace {

constexpr wchar_t service_window_class[] = L"LlavonImeServiceTrayWindow";
constexpr wchar_t service_shutdown_message[] = L"LlavonIme.Shutdown";
constexpr LRESULT shutdown_acknowledged = 0x4c4c4156;
constexpr DWORD graceful_shutdown_timeout_ms = 30000;
constexpr DWORD forced_shutdown_timeout_ms = 5000;

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

            // Older installed versions do not understand LlavonIme.Shutdown.
            // They have no mutable in-memory state, so use a bounded fallback
            // to make the first upgrade capable of replacing service binaries.
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
         std::wstring_view(arguments[1]) != L"prepare-uninstall")) {
        if (arguments) {
            LocalFree(arguments);
        }
        return ERROR_INVALID_PARAMETER;
    }

    const std::wstring service_path = arguments[2];
    LocalFree(arguments);

    const HRESULT stop_result = stop_service(service_path);
    if (FAILED(stop_result)) {
        return exit_code_from_hresult(stop_result);
    }

    ComApartment apartment;
    if (FAILED(apartment.result())) {
        return exit_code_from_hresult(apartment.result());
    }

    return exit_code_from_hresult(release_text_service());
}
