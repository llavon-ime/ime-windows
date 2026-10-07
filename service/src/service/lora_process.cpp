#include "lora_process.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <system_error>
#include <type_traits>

namespace llavon::service {
namespace {

struct CloseHandleDeleter {
    void operator()(void* handle) const noexcept { if (handle) CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, CloseHandleDeleter>;

[[noreturn]] void fail(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

Handle checked_handle(HANDLE handle) {
    if (!handle || handle == INVALID_HANDLE_VALUE) fail("LoRA process handle creation failed");
    return Handle(handle);
}

void check_cancelled(const std::atomic_bool& cancelling) {
    if (cancelling.load(std::memory_order_acquire))
        throw std::runtime_error("operation cancelled");
}

std::wstring quote_argument(std::wstring_view argument) {
    if (argument.empty()) return L"\"\"";
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

struct DeleteAttributes {
    void operator()(LPPROC_THREAD_ATTRIBUTE_LIST value) const noexcept {
        if (value) DeleteProcThreadAttributeList(value);
    }
};

struct ProcessTree {
    Handle job;
    Handle process;
    ~ProcessTree() {
        // Closing the only job handle also works during stack unwinding. The
        // child never inherits this handle, even when it launches descendants.
        job.reset();
        if (process) (void)WaitForSingleObject(process.get(), 5000);
    }
};

} // namespace

std::uint32_t run_lora_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::atomic_bool& cancelling,
    const std::function<void(std::string_view)>& on_line) {
    check_cancelled(cancelling);
    ProcessTree tree{checked_handle(CreateJobObjectW(nullptr, nullptr)), {}};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(tree.job.get(), JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        fail("unable to configure LoRA process job");

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_raw = nullptr;
    HANDLE write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &security, 0))
        fail("unable to create trainer output pipe");
    Handle read_pipe(read_raw);
    Handle write_pipe(write_raw);
    if (!SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0))
        fail("unable to protect trainer output pipe");
    auto input = checked_handle(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));

    // Assign the job atomically at creation, without a crash window between
    // CreateProcess and AssignProcessToJobObject. Restrict inherited handles
    // so neither the job nor unrelated service handles can escape to the child.
    SIZE_T attribute_size = 0;
    (void)InitializeProcThreadAttributeList(nullptr, 2, 0, &attribute_size);
    std::vector<std::byte> storage(attribute_size);
    auto* raw_attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(raw_attributes, 2, 0, &attribute_size))
        fail("unable to initialize trainer process attributes");
    std::array<HANDLE, 2> inherited{write_pipe.get(), input.get()};
    HANDLE job = tree.job.get();
    std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, DeleteAttributes>
        attributes(raw_attributes);
    if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                    inherited.data(), sizeof(inherited), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_JOB_LIST,
                                    &job, sizeof(job), nullptr, nullptr))
        fail("unable to bind trainer process ownership");

    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.StartupInfo.wShowWindow = SW_HIDE;
    startup.StartupInfo.hStdOutput = write_pipe.get();
    startup.StartupInfo.hStdError = write_pipe.get();
    startup.StartupInfo.hStdInput = input.get();
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION process{};
    const auto working_directory = executable.parent_path().wstring();
    check_cancelled(cancelling);
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        working_directory.c_str(), &startup.StartupInfo, &process))
        fail("unable to start LoRA process");
    tree.process.reset(process.hProcess);
    Handle thread(process.hThread);
    write_pipe.reset();
    input.reset();

    std::string pending;
    std::array<char, 4096> buffer{};
    bool exited = false;
    bool pipe_closed = false;
    for (;;) {
        check_cancelled(cancelling);
        const DWORD wait = WaitForSingleObject(tree.process.get(), 0);
        if (wait == WAIT_FAILED) fail("unable to query LoRA process");
        if (!exited && wait == WAIT_OBJECT_0) {
            exited = true;
            // A descendant may still own stdout after the direct child exits.
            // It must not keep this operation alive or continue training.
            if (!TerminateJobObject(tree.job.get(), ERROR_PROCESS_ABORTED))
                fail("unable to stop LoRA descendants");
        }
        DWORD available = 0;
        if (!pipe_closed && !PeekNamedPipe(read_pipe.get(), nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() != ERROR_BROKEN_PIPE) fail("unable to inspect trainer output");
            pipe_closed = true;
        }
        if (available != 0) {
            DWORD read = 0;
            if (!ReadFile(read_pipe.get(), buffer.data(),
                          std::min(available, static_cast<DWORD>(buffer.size())), &read, nullptr))
                fail("unable to read trainer output");
            for (const char character : std::string_view(buffer.data(), read)) {
                if (character == '\r' || character == '\n') {
                    if (!pending.empty()) on_line(pending);
                    pending.clear();
                } else {
                    if (pending.size() >= 64 * 1024)
                        throw std::runtime_error("LoRA output line is too large");
                    pending.push_back(character);
                }
            }
            continue;
        }
        if (exited) break;
        // Never block in ReadFile waiting for a newline or an inherited writer.
        // Polling the process keeps silent native-code hangs cancellable too.
        if (WaitForSingleObject(tree.process.get(), 50) == WAIT_FAILED)
            fail("unable to wait for LoRA process");
    }
    if (!pending.empty()) on_line(pending);
    check_cancelled(cancelling);
    DWORD exit_code = ERROR_GEN_FAILURE;
    if (!GetExitCodeProcess(tree.process.get(), &exit_code))
        fail("unable to read LoRA exit status");
    return exit_code;
}

} // namespace llavon::service
