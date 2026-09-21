#include "serviceLauncher.hpp"

#include "globals.h"

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace tsf {
namespace {

class UniqueHandle final {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&&) = delete;

    HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept {
        return handle_ && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_ = nullptr;
};

std::optional<std::filesystem::path> module_directory() {
    if (!Globals::hinstance) {
        return std::nullopt;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            Globals::hinstance, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return std::nullopt;
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) {
        return std::nullopt;
    }

    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0) {
        return std::nullopt;
    }
    value.resize(copied);
    return std::filesystem::path(value);
}

std::optional<std::filesystem::path> service_executable_path() {
    std::error_code error;
    if (auto configured = environment_path(L"LLAVON_IME_SERVICE_PATH")) {
        if (std::filesystem::is_regular_file(*configured, error)) {
            return configured;
        }
    }
    if (auto directory = module_directory()) {
        auto candidate = *directory / "llavon-ime-service.exe";
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

UniqueHandle interactive_shell_process() noexcept {
    const HWND shell_window = GetShellWindow();
    if (!shell_window) {
        return {};
    }

    DWORD shell_process_id = 0;
    GetWindowThreadProcessId(shell_window, &shell_process_id);
    if (shell_process_id == 0) {
        return {};
    }

    DWORD current_session = 0;
    DWORD shell_session = 0;
    if (!ProcessIdToSessionId(GetCurrentProcessId(), &current_session) ||
        !ProcessIdToSessionId(shell_process_id, &shell_session) ||
        current_session != shell_session) {
        return {};
    }

    return UniqueHandle(OpenProcess(PROCESS_CREATE_PROCESS | PROCESS_QUERY_LIMITED_INFORMATION,
                                    FALSE, shell_process_id));
}

bool create_service_process(const std::filesystem::path& executable,
                            HANDLE parent_process) {
    std::wstring command_line = L"\"" + executable.wstring() + L"\"";
    const std::wstring working_directory = executable.parent_path().wstring();
    PROCESS_INFORMATION process_info{};

    BOOL created = FALSE;
    if (parent_process) {
        SIZE_T attribute_bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        if (attribute_bytes != 0) {
            std::vector<std::byte> attribute_storage(attribute_bytes);
            auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                attribute_storage.data());
            if (InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_bytes)) {
                if (UpdateProcThreadAttribute(
                        attributes, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
                        &parent_process, sizeof(parent_process), nullptr, nullptr)) {
                    STARTUPINFOEXW startup_info{};
                    startup_info.StartupInfo.cb = sizeof(startup_info);
                    startup_info.lpAttributeList = attributes;
                    created = CreateProcessW(
                        nullptr, command_line.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        working_directory.empty() ? nullptr : working_directory.c_str(),
                        &startup_info.StartupInfo, &process_info);
                }
                DeleteProcThreadAttributeList(attributes);
            }
        }
    }

    // Keep service availability on systems where Explorer is absent or its
    // process cannot be opened. Such launches may not receive private band 16,
    // but preserve the existing backend behavior.
    if (!created) {
        command_line = L"\"" + executable.wstring() + L"\"";
        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);
        created = CreateProcessW(
            nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, working_directory.empty() ? nullptr : working_directory.c_str(),
            &startup_info, &process_info);
    }

    if (!created) {
        return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

}  // namespace

bool launch_service_backend() noexcept {
    try {
        const auto executable = service_executable_path();
        if (!executable) {
            return false;
        }
        return launch_process_with_shell_parent(*executable);
    } catch (...) {
        return false;
    }
}

bool launch_process_with_shell_parent(const std::filesystem::path& executable) noexcept {
    try {
        auto shell_process = interactive_shell_process();
        return create_service_process(executable, shell_process.get());
    } catch (...) {
        return false;
    }
}

}  // namespace tsf
