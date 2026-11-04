#include <llavon-ipc/pipe_listener.hpp>
#include <winternl.h>
#include "serviceLauncher.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

namespace {
using llavon::ipc::detail::LocalPtr;
using llavon::ipc::detail::UniqueHandle;

void require(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::format("{}: {}", operation, GetLastError()));
}

UniqueHandle appcontainer_token() {
    // Documented dynamically loaded API: no profile, account or Registry writes.
    using CreateLowBoxToken = NTSTATUS (NTAPI*)(PHANDLE, HANDLE, ACCESS_MASK,
        POBJECT_ATTRIBUTES, PSID, ULONG, PSID_AND_ATTRIBUTES, ULONG, HANDLE*);
    const auto create = reinterpret_cast<CreateLowBoxToken>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtCreateLowBoxToken"));
    require(create != nullptr, "resolve NtCreateLowBoxToken");
    HANDLE raw = nullptr;
    require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &raw) != FALSE,
            "open parent token");
    const UniqueHandle original(raw);
    PSID package_raw = nullptr;
    require(ConvertStringSidToSidW(L"S-1-15-2-111-222-333-444-555-666-777", &package_raw) != FALSE,
            "create test package SID");
    const LocalPtr<void> package(package_raw);
    OBJECT_ATTRIBUTES attributes{};
    attributes.Length = sizeof(attributes);
    const auto status = create(&raw, original.get(), TOKEN_ALL_ACCESS, &attributes,
        package.get(), 0, nullptr, 0, nullptr);
    if (status < 0) throw std::runtime_error(std::format("NtCreateLowBoxToken: 0x{:08x}",
        static_cast<std::uint32_t>(status)));
    return UniqueHandle(raw);
}

bool is_appcontainer(HANDLE token) {
    DWORD value = 0;
    DWORD size = sizeof(value);
    require(GetTokenInformation(token, TokenIsAppContainer, &value, size, &size) != FALSE,
            "query TokenIsAppContainer");
    return value != 0;
}

void transfer_byte(HANDLE pipe, bool write, char& byte, bool asynchronous) {
    const UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(static_cast<bool>(event), "create I/O event");
    OVERLAPPED operation{};
    operation.hEvent = event.get();
    DWORD count = 0;
    auto* pending = asynchronous ? &operation : nullptr;
    const BOOL success = write ? WriteFile(pipe, &byte, 1, &count, pending) :
                                ReadFile(pipe, &byte, 1, &count, pending);
    if (!success) {
        require(asynchronous && GetLastError() == ERROR_IO_PENDING, "start pipe I/O");
        const DWORD wait = WaitForSingleObject(event.get(), 5000);
        if (wait != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &operation);
            GetOverlappedResult(pipe, &operation, &count, TRUE);
            require(false, "pipe I/O timeout");
        }
        require(GetOverlappedResult(pipe, &operation, &count, FALSE) != FALSE, "finish pipe I/O");
    }
    require(count == 1, "one byte transferred");
}

class ChildProcess final {
public:
    ChildProcess(HANDLE token, std::wstring command) {
        std::wstring executable(32768, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
            static_cast<DWORD>(executable.size()));
        require(length > 0 && length < executable.size(), "test executable path");
        executable.resize(length);
        command = std::format(L"\"{}\" {}", executable, command);
        std::wstring directory(32768, L'\0');
        const UINT copied = GetSystemDirectoryW(directory.data(), static_cast<UINT>(directory.size()));
        require(copied > 0 && copied < directory.size(), "system working directory");
        directory.resize(copied);
        STARTUPINFOW startup{sizeof(STARTUPINFOW)};
        PROCESS_INFORMATION info{};
        require(CreateProcessAsUserW(token, executable.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &info) != FALSE,
            "start real AppContainer child");
        process_.reset(info.hProcess);
        const UniqueHandle thread(info.hThread);
    }
    ~ChildProcess() {
        // Only this test's own child is terminated if an assertion fails.
        if (WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_.get(), ERROR_CANCELLED);
            WaitForSingleObject(process_.get(), 5000);
        }
    }
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    HANDLE get() const noexcept { return process_.get(); }
    void verify_appcontainer() const {
        HANDLE raw = nullptr;
        require(OpenProcessToken(process_.get(), TOKEN_QUERY, &raw) != FALSE, "open child token");
        const UniqueHandle child_token(raw);
        require(is_appcontainer(child_token.get()), "child really is AppContainer");
    }
    DWORD exit_code() const {
        DWORD code = STILL_ACTIVE;
        require(GetExitCodeProcess(process_.get(), &code) != FALSE, "child exit code");
        return code;
    }
private:
    UniqueHandle process_;
};

HANDLE WINAPI legacy_create(LPCWSTR name, DWORD access, DWORD mode, DWORD instances,
                           DWORD out_size, DWORD in_size, DWORD timeout, LPSECURITY_ATTRIBUTES) {
    const auto user = llavon::ipc::detail::current_user_sid();
    const auto sid = llavon::ipc::detail::sid_string(const_cast<std::byte*>(user.data()));
    // Model the normal Medium IL legacy backend even on elevated CI runners.
    const auto sddl = std::format(
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;{})(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)S:(ML;;NW;;;ME)", sid.get());
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &raw, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    const LocalPtr<void> descriptor(raw);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    return CreateNamedPipeW(name, access, mode, instances, out_size, in_size, timeout, &security);
}

HANDLE WINAPI unpackaged_only_create(LPCWSTR name, DWORD access, DWORD mode, DWORD instances,
                                    DWORD out_size, DWORD in_size, DWORD timeout, LPSECURITY_ATTRIBUTES) {
    PSECURITY_DESCRIPTOR raw = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;WD)S:(ML;;NW;;;LW)", SDDL_REVISION_1, &raw, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    const LocalPtr<void> descriptor(raw);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    return CreateNamedPipeW(name, access, mode, instances, out_size, in_size, timeout, &security);
}

void test_package_grant_required(HANDLE token) {
    const auto base = std::format(L"llavon-appcontainer-denied-{}", GetCurrentProcessId());
    llavon::ipc::PipeListener listener(llavon::ipc::session_pipe_name(base), PIPE_ACCESS_DUPLEX,
        4096, &unpackaged_only_create);
    require(listener.ensure() == ERROR_SUCCESS, "create pipe without package grant");
    SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const UniqueHandle consumed(CreateEventW(&inherited, TRUE, FALSE, nullptr));
    require(static_cast<bool>(consumed), "create denial completion event");
    const ChildProcess child(token, std::format(L"--client {} false true true {}", base,
        reinterpret_cast<std::uintptr_t>(consumed.get())));
    require(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0, "denied AppContainer child exits");
    require(child.exit_code() == ERROR_ACCESS_DENIED, "Everyone/Low label alone does not grant AppContainer access");
    std::puts("PASS: real AppContainer process is denied without a package grant, even with Everyone/Low label");
}

void test_process(HANDLE token, bool legacy_server, bool legacy_client, bool duplex, bool asynchronous) {
    static unsigned serial = 0;
    const auto base = std::format(L"llavon-appcontainer-test-{}-{}", GetCurrentProcessId(), ++serial);
    const auto name = llavon::ipc::session_pipe_name(base);
    llavon::ipc::PipeListener listener(name, duplex ? PIPE_ACCESS_DUPLEX : PIPE_ACCESS_INBOUND,
        4096, legacy_server ? &legacy_create : &CreateNamedPipeW);
    require(listener.ensure() == ERROR_SUCCESS, "create AppContainer test listener");
    const UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(static_cast<bool>(event), "create connection event");
    llavon::ipc::PendingPipeConnection pending(listener.get(), event.get());
    require(pending.start() == ERROR_IO_PENDING, "start accept before child launch");
    SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    const UniqueHandle consumed(CreateEventW(&inherited, TRUE, FALSE, nullptr));
    require(static_cast<bool>(consumed), "create inherited completion event");
    const auto command = std::format(L"--client {} {} {} {} {}", base, legacy_client, duplex,
        asynchronous, reinterpret_cast<std::uintptr_t>(consumed.get()));
    const ChildProcess child(token, command);
    child.verify_appcontainer();
    const std::array<HANDLE, 2> waits{event.get(), child.get()};
    if (WaitForMultipleObjects(static_cast<DWORD>(waits.size()), waits.data(), FALSE, 5000) != WAIT_OBJECT_0) {
        throw std::runtime_error(std::format(
            "AppContainer connection failed: legacy_server={} legacy_client={} duplex={} async={} child_exit={}",
            legacy_server, legacy_client, duplex, asynchronous, child.exit_code()));
    }
    require(pending.complete() == ERROR_SUCCESS, "accept real AppContainer child");
    auto accepted = listener.take_connected();
    require(accepted.has_value(), "replace listener before serving AppContainer");
    char byte = 'p';
    if (duplex) transfer_byte(accepted->get(), true, byte, true);
    transfer_byte(accepted->get(), false, byte, true);
    require(byte == 'c', "receive AppContainer response");
    require(SetEvent(consumed.get()) != FALSE, "release child after receiving data");
    require(WaitForSingleObject(child.get(), 5000) == WAIT_OBJECT_0, "AppContainer child exits");
    require(child.exit_code() == ERROR_SUCCESS, "AppContainer child succeeded");
}

int client(int argc, wchar_t* argv[]) {
    require(argc == 7, "client arguments");
    require(is_appcontainer(GetCurrentProcessToken()), "client has AppContainer process token");
    const bool legacy = std::wstring_view(argv[3]) == L"true";
    const bool duplex = std::wstring_view(argv[4]) == L"true";
    const bool asynchronous = std::wstring_view(argv[5]) == L"true";
    const auto consumed = reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(std::wcstoull(argv[6], nullptr, 10)));
    const auto name = llavon::ipc::session_pipe_name(argv[2]);
    const DWORD access = legacy ? (duplex ? GENERIC_READ | GENERIC_WRITE : GENERIC_WRITE) :
        (duplex ? llavon::ipc::pipe_client_read_write_access : llavon::ipc::pipe_client_write_access);
    const DWORD flags = (asynchronous ? FILE_FLAG_OVERLAPPED : 0) |
        (legacy ? 0 : llavon::ipc::pipe_client_security_flags);
    const UniqueHandle pipe(legacy ?
        CreateFileW(name.c_str(), access, 0, nullptr, OPEN_EXISTING, flags, nullptr) :
        tsf::connect_service_pipe(argv[2], access, flags));
    if (pipe.get() == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
    char byte = 0;
    if (duplex) {
        transfer_byte(pipe.get(), false, byte, asynchronous);
        require(byte == 'p', "receive server request");
    }
    byte = 'c';
    transfer_byte(pipe.get(), true, byte, asynchronous);
    require(WaitForSingleObject(consumed, 5000) == WAIT_OBJECT_0, "server consumes AppContainer data");
    return 0;
}
}  // namespace

int wmain(int argc, wchar_t* argv[]) try {
    if (argc > 1) return client(argc, argv);
    const auto token = appcontainer_token();
    require(is_appcontainer(token.get()), "lowbox token is AppContainer");
    test_package_grant_required(token.get());
    for (const bool legacy_server : {true, false}) {
        for (const bool legacy_client : {true, false}) {
            test_process(token.get(), legacy_server, legacy_client, true, true);
            test_process(token.get(), legacy_server, legacy_client, false, false);
            test_process(token.get(), legacy_server, legacy_client, false, true);
            std::printf("PASS: real AppContainer process: %s server / %s client; prediction, candidate, logger I/O\n",
                legacy_server ? "legacy" : "current", legacy_client ? "legacy" : "current");
        }
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
