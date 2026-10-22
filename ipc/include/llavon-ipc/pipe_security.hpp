#pragma once

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>

#include <cstddef>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace llavon::ipc {

namespace detail {

struct LocalFreeDeleter {
    void operator()(void* memory) const noexcept { LocalFree(memory); }
};

template <typename T>
using LocalPtr = std::unique_ptr<T, LocalFreeDeleter>;

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

// SearchHost and other modern Windows text controls run in an
// AppContainer.  The default pipe DACL does not grant AppContainer
// tokens access, so those hosts otherwise receive ERROR_ACCESS_DENIED
// and block their TSF UI thread while the frontend retries.
class PipeSecurity final {
public:
    PipeSecurity() noexcept {
        try {
            const auto sid = detail::current_user_sid();
            if (sid.empty()) return;

            wchar_t* raw_sid = nullptr;
            if (!ConvertSidToStringSidW(const_cast<std::byte*>(sid.data()), &raw_sid)) return;
            const detail::LocalPtr<wchar_t> sid_string(raw_sid);

            const auto sddl = std::format(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;{})(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)",
                sid_string.get());
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) {
                return;
            }
            descriptor_.reset(descriptor);
            attributes_.lpSecurityDescriptor = descriptor;
        } catch (...) {
        }
    }

    explicit operator bool() const noexcept { return descriptor_ != nullptr; }

    SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }

private:
    detail::LocalPtr<void> descriptor_;
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};

inline bool is_trusted_pipe_server(HANDLE pipe) noexcept {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    if (GetSecurityInfo(pipe, SE_KERNEL_OBJECT, OWNER_SECURITY_INFORMATION, &owner,
                        nullptr, nullptr, nullptr, &raw_descriptor) != ERROR_SUCCESS) {
        return false;
    }
    const detail::LocalPtr<void> descriptor(raw_descriptor);
    if (!owner || !IsValidSid(owner)) return false;
    if (IsWellKnownSid(owner, WinLocalSystemSid) ||
        IsWellKnownSid(owner, WinBuiltinAdministratorsSid)) {
        return true;
    }

    auto user = detail::current_user_sid();
    return !user.empty() && EqualSid(owner, user.data());
}

}  // namespace llavon::ipc
