#include <llavon-ipc/pipe_security.hpp>
#include <aclapi.h>
#include <authz.h>

#include <array>
#include <cstdio>
#include <stdexcept>
#include <thread>

namespace {
using llavon::ipc::detail::LocalPtr;
using llavon::ipc::detail::UniqueHandle;

void require(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::format("{}: {}", operation, GetLastError()));
}

LocalPtr<void> sid(const wchar_t* text) {
    PSID raw = nullptr;
    require(ConvertStringSidToSidW(text, &raw) != FALSE, "parse SID");
    return LocalPtr<void>(raw);
}

LocalPtr<void> descriptor(const std::wstring& text) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        text.c_str(), SDDL_REVISION_1, &raw, nullptr) != FALSE, "parse descriptor");
    return LocalPtr<void>(raw);
}

LocalPtr<void> object_descriptor(HANDLE object) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    const DWORD error = GetSecurityInfo(object, SE_KERNEL_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &raw);
    require(error == ERROR_SUCCESS, "read mapped object descriptor");
    return LocalPtr<void>(raw);
}

class Impersonation final {
public:
    explicit Impersonation(HANDLE token) {
        require(SetThreadToken(nullptr, token) != FALSE, "impersonate test token");
    }
    ~Impersonation() { RevertToSelf(); }
    Impersonation(const Impersonation&) = delete;
    Impersonation& operator=(const Impersonation&) = delete;
};

UniqueHandle low_token(HANDLE original, DWORD integrity) {
    HANDLE raw = nullptr;
    require(DuplicateTokenEx(original, TOKEN_QUERY | TOKEN_ADJUST_DEFAULT | TOKEN_IMPERSONATE,
        nullptr, SecurityImpersonation, TokenImpersonation, &raw) != FALSE, "duplicate token");
    UniqueHandle token(raw);
    const auto level = sid(std::format(L"S-1-16-{}", integrity).c_str());
    TOKEN_MANDATORY_LABEL label{{level.get(), SE_GROUP_INTEGRITY}};
    require(SetTokenInformation(token.get(), TokenIntegrityLevel, &label,
        static_cast<DWORD>(sizeof(label)) + GetLengthSid(level.get())) != FALSE, "lower integrity");
    return token;
}

void connect_as(HANDLE token, SECURITY_ATTRIBUTES* security, bool allowed,
                DWORD server_access = PIPE_ACCESS_DUPLEX) {
    static unsigned serial = 0;
    const auto name = std::format(L"\\\\.\\pipe\\llavon-security-test-{}-{}",
                                  GetCurrentProcessId(), ++serial);
    const UniqueHandle server(CreateNamedPipeW(name.c_str(), server_access,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, security));
    require(server.get() != INVALID_HANDLE_VALUE, "create test pipe");
    const Impersonation impersonation(token);
    const auto access = server_access == PIPE_ACCESS_INBOUND ? llavon::ipc::pipe_client_write_access :
        llavon::ipc::pipe_client_read_write_access;
    const HANDLE raw = CreateFileW(name.c_str(), access, 0, nullptr,
        OPEN_EXISTING, llavon::ipc::pipe_client_security_flags, nullptr);
    if (!allowed) {
        require(raw == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED,
                "pipe must reject this integrity level");
        return;
    }
    require(raw != INVALID_HANDLE_VALUE, "connect to labelled pipe");
    const UniqueHandle client(raw);
    constexpr char sent = 'x';
    DWORD written = 0;
    require(WriteFile(client.get(), &sent, sizeof(sent), &written, nullptr) && written == 1,
            "write from sandboxed client");
    char received = 0;
    DWORD read = 0;
    require(ReadFile(server.get(), &received, sizeof(received), &read, nullptr) && received == sent,
            "server receives sandboxed input");
}

// Authz contexts exercise the DACL with identities unavailable on the machine.
// These model UAC/other-account and package/restricting SIDs, not live AppContainers.
class Authorization final {
public:
    Authorization() {
        require(AuthzInitializeResourceManager(AUTHZ_RM_FLAG_NO_AUDIT, nullptr, nullptr, nullptr,
            L"Llavon pipe security tests", &manager_) != FALSE, "create Authz manager");
    }
    ~Authorization() { AuthzFreeResourceManager(manager_); }
    Authorization(const Authorization&) = delete;
    Authorization& operator=(const Authorization&) = delete;

    bool allows(PSECURITY_DESCRIPTOR security, PSID user, PSID group = nullptr,
                DWORD group_attributes = SE_GROUP_ENABLED, PSID restricting = nullptr,
                DWORD access = llavon::ipc::pipe_client_read_write_access,
                PSID deny_only_group = nullptr) const {
        AUTHZ_CLIENT_CONTEXT_HANDLE original = nullptr;
        const LUID identifier{};
        require(AuthzInitializeContextFromSid(AUTHZ_SKIP_TOKEN_GROUPS, user, manager_, nullptr,
            identifier, nullptr, &original) != FALSE, "create Authz identity");
        AUTHZ_CLIENT_CONTEXT_HANDLE context = original;
        std::array<SID_AND_ATTRIBUTES, 2> normal{};
        DWORD group_count = 0;
        if (group) normal[group_count++] = {group, group_attributes};
        if (deny_only_group) normal[group_count++] = {deny_only_group, SE_GROUP_USE_FOR_DENY_ONLY};
        SID_AND_ATTRIBUTES restricted{restricting, SE_GROUP_ENABLED};
        if (group_count || restricting) {
            const BOOL added = AuthzAddSidsToContext(original, normal.data(), group_count,
                restricting ? &restricted : nullptr, restricting ? 1 : 0, &context);
            AuthzFreeContext(original);
            require(added != FALSE, "add group/restricting SIDs");
        }
        DWORD granted = 0;
        DWORD error = ERROR_SUCCESS;
        AUTHZ_ACCESS_REQUEST request{};
        request.DesiredAccess = access;
        AUTHZ_ACCESS_REPLY reply{};
        reply.ResultListLength = 1;
        reply.GrantedAccessMask = &granted;
        reply.Error = &error;
        const BOOL checked = AuthzAccessCheck(0, context, &request, nullptr, security,
            nullptr, 0, &reply, nullptr);
        AuthzFreeContext(context);
        require(checked != FALSE, "AuthzAccessCheck");
        return error == ERROR_SUCCESS && (granted & access) == access;
    }
private:
    AUTHZ_RESOURCE_MANAGER_HANDLE manager_ = nullptr;
};

void test_access_matrix() {
    const Authorization authorization;
    const auto user = llavon::ipc::detail::current_user_sid();
    const auto stranger = sid(L"S-1-5-21-111-222-333-1001");
    const auto admins = sid(L"S-1-5-32-544");
    const auto users = sid(L"S-1-5-32-545");
    const auto system = sid(L"S-1-5-18");
    const auto ac = sid(L"S-1-15-2-1");
    const auto lpac = sid(L"S-1-15-2-2");
    const auto rc = sid(L"S-1-5-12");
    llavon::ipc::PipeSecurity security;
    require(static_cast<bool>(security), "pipe descriptor");
    const auto name = std::format(L"\\\\.\\pipe\\llavon-access-matrix-{}", GetCurrentProcessId());
    const UniqueHandle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0,
        security.attributes()));
    require(pipe.get() != INVALID_HANDLE_VALUE, "create matrix pipe");
    // Authz expects object-specific ACE masks. The kernel maps GA/GR/GW/GX
    // when creating the object; use that descriptor instead of raw SDDL.
    const auto mapped_pipe = object_descriptor(pipe.get());
    const auto sd = mapped_pipe.get();
    auto* user_sid = const_cast<std::byte*>(user.data());
    require(authorization.allows(sd, user_sid), "same user");
    require(authorization.allows(sd, user_sid, admins.get(), SE_GROUP_USE_FOR_DENY_ONLY),
            "same user with UAC-filtered Administrators");
    require(authorization.allows(sd, stranger.get(), admins.get()), "another elevated administrator");
    require(!authorization.allows(sd, stranger.get(), admins.get(), SE_GROUP_USE_FOR_DENY_ONLY),
            "deny-only Administrators cannot grant access");
    require(!authorization.allows(sd, stranger.get()), "unrelated user is denied");
    require(authorization.allows(sd, stranger.get(), users.get()), "another local Users account");
    require(authorization.allows(sd, stranger.get(), users.get(), SE_GROUP_ENABLED, nullptr,
        llavon::ipc::pipe_client_read_write_access, admins.get()),
        "another UAC-filtered administrator is allowed through Users");
    require(authorization.allows(sd, stranger.get(), users.get(), SE_GROUP_ENABLED, nullptr,
        FILE_GENERIC_READ | FILE_GENERIC_WRITE), "legacy generic client access");
    for (const DWORD denied : std::array<DWORD, 2>{WRITE_DAC, WRITE_OWNER}) {
        require(!authorization.allows(sd, stranger.get(), users.get(), SE_GROUP_ENABLED, nullptr, denied),
                "local client cannot change pipe security");
    }
    require(authorization.allows(sd, system.get()), "LocalSystem");
    for (const auto& sandbox : {ac.get(), lpac.get(), rc.get()}) {
        require(authorization.allows(sd, user_sid, nullptr, 0, sandbox), "sandbox restricting SID");
    }
    require(!authorization.allows(sd, user_sid, nullptr, 0, stranger.get()), "unknown restriction");

    llavon::ipc::SharedObjectSecurity mutex(llavon::ipc::SharedObject::Mutex);
    require(static_cast<bool>(mutex), "mutex descriptor");
    const UniqueHandle mutex_handle(CreateMutexExW(mutex.attributes(), nullptr, 0,
        READ_CONTROL | SYNCHRONIZE));
    require(static_cast<bool>(mutex_handle), "create matrix mutex");
    const auto mapped_mutex = object_descriptor(mutex_handle.get());
    const auto mutex_sd = mapped_mutex.get();
    require(authorization.allows(mutex_sd, stranger.get(), users.get(), SE_GROUP_ENABLED, nullptr,
        SYNCHRONIZE), "another Users account can open launch/singleton mutex");
    for (const auto& sandbox : {ac.get(), lpac.get(), rc.get()}) {
        require(authorization.allows(mutex_sd, user_sid, nullptr, 0, sandbox,
            SYNCHRONIZE), "sandbox can wait/release mutex");
        require(!authorization.allows(mutex_sd, user_sid, nullptr, 0, sandbox, WRITE_DAC),
                "sandbox cannot rewrite mutex DACL");
    }
    std::puts("PASS: Authz model: Users/admin/SYSTEM/package DACL matrix; legacy generic access; client cannot change security");
}

void test_live_objects() {
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw) != FALSE,
            "open process token");
    const UniqueHandle process_token(raw);
    const auto low = low_token(process_token.get(), SECURITY_MANDATORY_LOW_RID);
    const auto untrusted = low_token(process_token.get(), SECURITY_MANDATORY_UNTRUSTED_RID);
    llavon::ipc::PipeSecurity security;
    require(static_cast<bool>(security), "live pipe security");

    // A plain Low IL token is not an AppContainer token. Without a package
    // grant, MIC still rejects writes to a Medium IL object despite user access.
    const auto user = llavon::ipc::detail::current_user_sid();
    const auto text = llavon::ipc::detail::sid_string(const_cast<std::byte*>(user.data()));
    const auto previous = descriptor(std::format(L"D:P(A;;GA;;;{})", text.get()));
    SECURITY_ATTRIBUTES old{sizeof(SECURITY_ATTRIBUTES), previous.get(), FALSE};
    connect_as(low.get(), &old, false);

    connect_as(low.get(), security.attributes(), true);
    connect_as(low.get(), security.attributes(), true, PIPE_ACCESS_INBOUND);
    std::puts("PASS: live Low IL prediction/candidate pipe I/O");
    // The Low/NW policy does not grant the still lower Untrusted level.
    connect_as(untrusted.get(), security.attributes(), false);

    const auto name = std::format(L"Local\\LlavonSecurityTest-{}", GetCurrentProcessId());
    const UniqueHandle first(llavon::ipc::create_shared_mutex(name.c_str()));
    require(static_cast<bool>(first), "create shared launch mutex");
    {
        const Impersonation impersonation(low.get());
        const UniqueHandle second(llavon::ipc::create_shared_mutex(name.c_str()));
        require(static_cast<bool>(second), "open existing launch mutex from Low IL");
        require(WaitForSingleObject(second.get(), 0) == WAIT_OBJECT_0, "acquire launch mutex");
        require(ReleaseMutex(second.get()) != FALSE, "release launch mutex");
    }
    const auto singleton_name = name + L"-singleton";
    const UniqueHandle singleton(llavon::ipc::create_shared_mutex(singleton_name.c_str(), true));
    require(static_cast<bool>(singleton) && GetLastError() != ERROR_ALREADY_EXISTS, "claim singleton");
    DWORD second_error = ERROR_SUCCESS;
    DWORD second_wait = WAIT_FAILED;
    std::thread contender([&] {
        const UniqueHandle second(llavon::ipc::create_shared_mutex(singleton_name.c_str(), true));
        second_error = GetLastError();
        if (second) second_wait = WaitForSingleObject(second.get(), 0);
    });
    contender.join();
    require(second_error == ERROR_ALREADY_EXISTS && second_wait == WAIT_TIMEOUT,
            "second backend sees existing singleton and cannot acquire it");
    require(ReleaseMutex(singleton.get()) != FALSE, "release test singleton");
    std::puts("PASS: plain Low IL MIC denial reproduced; Untrusted denied; shared mutex acquired/released from Low IL");
}
}  // namespace

int main() try {
    test_access_matrix();
    test_live_objects();
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
