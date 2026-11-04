#pragma once

#include <windows.h>
#include <filesystem>
#include <string_view>

namespace tsf {

inline constexpr wchar_t service_launch_mutex_name[] = L"Local\\LlavonImeBackendStart";

// Opens the session pipe with the requested client access and SQOS flags.
HANDLE connect_service_pipe(std::wstring_view base_name, DWORD access, DWORD flags) noexcept;

// Starts the backend with the interactive shell as its logical parent whenever
// possible. The shell-created process context is required by the candidate
// window's undocumented window-band integration.
bool launch_service_backend() noexcept;

// Used by the installer lifecycle helper after it has replaced the service.
bool launch_process_with_shell_parent(const std::filesystem::path& executable) noexcept;

}  // namespace tsf
