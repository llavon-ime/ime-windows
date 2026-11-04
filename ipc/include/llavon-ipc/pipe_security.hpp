#pragma once

// Security policies adapted from Google Mozc's base/win32/win_sandbox.cc.
// Copyright 2010-2021, Google Inc. All rights reserved.
// See ipc/MOZC-LICENSE.txt for the BSD 3-Clause license.
// Cross-account Users access follows azooKey-Windows (ipc/AZOOKEY-LICENSE.txt).
// Explicit client data rights follow PowerToys (ipc/POWERTOYS-LICENSE.txt).

#include <windows.h>
#include <sddl.h>

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llavon::ipc {

namespace detail {

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

using UniqueHandle = std::unique_ptr<void, CloseHandleDeleter>;

struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept { LocalFree(memory); }
};

template <typename T>
using LocalPtr = std::unique_ptr<T, LocalFreeDeleter>;

inline std::vector<std::byte> token_information(HANDLE token, TOKEN_INFORMATION_CLASS type) {
    DWORD size = 0;
    GetTokenInformation(token, type, nullptr, 0, &size);
    if (size == 0) return {};
    std::vector<std::byte> data(size);
    if (!GetTokenInformation(token, type, data.data(), size, &size)) return {};
    return data;
}

inline LocalPtr<wchar_t> sid_string(PSID sid) noexcept {
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(sid, &text)) return {};
    return LocalPtr<wchar_t>(text);
}

inline std::vector<std::byte> current_user_sid() noexcept {
    try {
        DWORD size = 0;
        GetTokenInformation(GetCurrentProcessToken(), TokenUser, nullptr, 0, &size);
        if (size == 0) return {};
        std::vector<std::byte> token_user(size);
        if (!GetTokenInformation(
                GetCurrentProcessToken(), TokenUser, token_user.data(), size, &size)) {
            return {};
        }

        const PSID user = reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid;
        const DWORD sid_size = GetLengthSid(user);
        std::vector<std::byte> sid(sid_size);
        if (!CopySid(sid_size, sid.data(), user)) return {};
        return sid;
    } catch (...) {
        return {};
    }
}

}  // namespace detail

inline std::wstring session_pipe_name(std::wstring_view base_name) noexcept {
    try {
        DWORD session_id = 0;
        if (!ProcessIdToSessionId(GetCurrentProcessId(), &session_id)) return {};
        return std::format(L"\\\\.\\pipe\\{}-{}", base_name, session_id);
    } catch (...) {
        return {};
    }
}

enum class SharedObject { Pipe, Mutex };

// New clients request explicit data rights. The server ACL must also accept
// legacy GENERIC_READ/GENERIC_WRITE clients still loaded in TSF hosts.
inline constexpr DWORD pipe_client_read_access =
    FILE_READ_DATA | FILE_READ_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
inline constexpr DWORD pipe_client_write_access =
    FILE_WRITE_DATA | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES | READ_CONTROL | SYNCHRONIZE;
inline constexpr DWORD pipe_client_read_write_access = pipe_client_read_access | pipe_client_write_access;
static_assert((pipe_client_read_write_access & FILE_CREATE_PIPE_INSTANCE) == 0);

// One backend per desktop session. BU allows another local user (including a
// UAC-filtered administrator) to use it. Package/restricting SIDs also need a
// grant. Preserve generic read/write grants for already loaded legacy clients.
class SharedObjectSecurity final {
public:
    explicit SharedObjectSecurity(SharedObject type = SharedObject::Pipe) noexcept {
        try {
            HANDLE raw_token = nullptr;
            if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &raw_token)) {
                error_ = GetLastError();
                if (error_ != ERROR_NO_TOKEN) return;
                if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
                    error_ = GetLastError();
                    return;
                }
            }
            const detail::UniqueHandle token(raw_token);
            const auto user_info = detail::token_information(token.get(), TokenUser);
            if (user_info.empty()) { error_ = GetLastError(); return; }
            const auto group_info = detail::token_information(token.get(), TokenPrimaryGroup);
            if (group_info.empty()) { error_ = GetLastError(); return; }
            const auto user = detail::sid_string(
                reinterpret_cast<const TOKEN_USER*>(user_info.data())->User.Sid);
            if (!user) { error_ = GetLastError(); return; }
            const auto group = detail::sid_string(
                reinterpret_cast<const TOKEN_PRIMARY_GROUP*>(group_info.data())->PrimaryGroup);
            if (!group) { error_ = GetLastError(); return; }

            const auto client_access = type == SharedObject::Pipe ? L"GRGW" : L"GX";
            const auto sddl = std::format(
                L"O:{}G:{}D:P(A;;;;;OW)(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;{})"
                L"(A;;{};;;BU)(A;;{};;;AC)(A;;{};;;S-1-15-2-2)(A;;{};;;RC)S:(ML;;NW;;;LW)",
                user.get(), group.get(), user.get(), client_access, client_access, client_access,
                client_access);
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
                error_ = GetLastError();
                return;
            }
            descriptor_.reset(descriptor);
            attributes_.lpSecurityDescriptor = descriptor;
            error_ = ERROR_SUCCESS;
        } catch (...) {
            error_ = ERROR_NOT_ENOUGH_MEMORY;
        }
    }

    explicit operator bool() const noexcept { return descriptor_ != nullptr; }
    DWORD error() const noexcept { return error_ == ERROR_SUCCESS && !descriptor_ ? ERROR_INVALID_SECURITY_DESCR : error_; }

    SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }

private:
    detail::LocalPtr<void> descriptor_;
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
    DWORD error_ = ERROR_INVALID_SECURITY_DESCR;
};

using PipeSecurity = SharedObjectSecurity;

// CreateMutexW asks for MUTEX_ALL_ACCESS even when opening an existing mutex.
// Sandboxed clients only need to wait and release, as in Mozc's mutex policy.
inline HANDLE create_shared_mutex(const wchar_t* name, bool initially_owned = false) noexcept {
    HANDLE mutex = nullptr;
    DWORD error = ERROR_NOT_ENOUGH_MEMORY;
    {
        SharedObjectSecurity security(SharedObject::Mutex);
        if (security) {
            // ReleaseMutex checks thread ownership; MUTEX_MODIFY_STATE is reserved.
            SetLastError(ERROR_SUCCESS);
            mutex = CreateMutexExW(security.attributes(), name,
                initially_owned ? CREATE_MUTEX_INITIAL_OWNER : 0, SYNCHRONIZE);
            error = GetLastError();
        } else {
            error = security.error();
        }
    }
    // The singleton caller needs ERROR_ALREADY_EXISTS after descriptor cleanup.
    SetLastError(error);
    return mutex;
}

inline constexpr DWORD pipe_client_security_flags =
    SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION | SECURITY_EFFECTIVE_ONLY;

}  // namespace llavon::ipc
