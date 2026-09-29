#include "npu_compiler_process.hpp"

#include <Windows.h>

#include <array>
#include <format>
#include <memory>
#include <stdexcept>
#include <vector>

namespace llavon::service::ryzen_ai::detail {
namespace {

struct HandleCloser {
    void operator()(void* handle) const noexcept { CloseHandle(handle); }
};
using Handle = std::unique_ptr<void, HandleCloser>;

[[noreturn]] void win32_error(const char* operation) {
    throw std::runtime_error(std::format("NPU compiler {} failed (Win32 {})", operation, GetLastError()));
}

std::wstring quote_argument(std::wstring_view argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') {
            ++slashes;
        } else {
            result.append(character == L'\"' ? 2 * slashes + 1 : slashes, L'\\');
            result += character;
            slashes = 0;
        }
    }
    result.append(2 * slashes, L'\\');
    result += L'\"';
    return result;
}

} // namespace

std::filesystem::path compiler_executable() {
    std::wstring path(512, L'\0');
    for (;;) {
        const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!size) win32_error("executable lookup");
        if (size < path.size()) {
            path.resize(size);
            return std::filesystem::path(path).parent_path() / L"llavon-ime-npu-compiler.exe";
        }
        path.resize(path.size() * 2);
    }
}

void run_compiler_process(const std::filesystem::path& executable,
                          std::span<const std::wstring> arguments,
                          const std::function<void(std::string)>& log_output) {
    if (!std::filesystem::is_regular_file(executable)) {
        throw std::runtime_error("Missing llavon-ime-npu-compiler.exe beside the service executable");
    }
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    if (!CreatePipe(&raw_read, &raw_write, &security, 0)) win32_error("output pipe");
    Handle read(raw_read), write(raw_write);
    if (!SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0)) win32_error("pipe inheritance");
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             &security, OPEN_EXISTING, 0, nullptr));
    if (input.get() == INVALID_HANDLE_VALUE) {
        input.release();
        win32_error("standard input");
    }
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<std::byte> storage(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &bytes)) win32_error("attribute list");
    struct AttributeGuard {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attributes_guard{attributes};
    std::array<HANDLE, 2> inherited{input.get(), write.get()};
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), sizeof(inherited), nullptr, nullptr)) {
        win32_error("handle list");
    }
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job) win32_error("job creation");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        win32_error("job setup");
    }
    auto command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) command += L" " + quote_argument(argument);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.get();
    startup.StartupInfo.hStdOutput = write.get();
    startup.StartupInfo.hStdError = write.get();
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process_info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                         CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT,
                         nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &process_info)) {
        win32_error("launch");
    }
    Handle process(process_info.hProcess), thread(process_info.hThread);
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        const auto error = GetLastError();
        TerminateProcess(process.get(), 1);
        SetLastError(error);
        win32_error("job assignment");
    }
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) win32_error("resume");
    write.reset();
    input.reset();
    std::array<char, 4096> buffer;
    std::string pending_line;
    std::string diagnostic_tail;
    constexpr size_t maximum_line = 16384;
    constexpr size_t maximum_tail = 8192;
    const auto is_utf8_continuation = [](char character) {
        return (static_cast<unsigned char>(character) & 0xC0) == 0x80;
    };
    const auto emit_line = [&] {
        if (!pending_line.empty() && pending_line.back() == '\r') pending_line.pop_back();
        if (log_output && !pending_line.empty()) log_output(pending_line);
        pending_line.clear();
    };
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(read.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            win32_error("output read");
        }
        if (!count) break;
        diagnostic_tail.append(buffer.data(), count);
        if (diagnostic_tail.size() > maximum_tail) {
            auto start = diagnostic_tail.size() - maximum_tail;
            while (start < diagnostic_tail.size() && is_utf8_continuation(diagnostic_tail[start])) ++start;
            diagnostic_tail.erase(0, start);
        }
        for (const char character : std::string_view(buffer.data(), count)) {
            if (character == '\n') {
                emit_line();
            } else {
                // Wait for the next code point before splitting a long line.
                if (pending_line.size() >= maximum_line && !is_utf8_continuation(character)) emit_line();
                pending_line += character;
            }
        }
    }
    emit_line(); // Preserve a final fatal diagnostic even without a newline.
    if (WaitForSingleObject(process.get(), INFINITE) != WAIT_OBJECT_0) win32_error("wait");
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process.get(), &exit_code)) win32_error("exit status");
    if (exit_code != 0) {
        throw std::runtime_error(std::format(
            "NPU compiler process exited with code 0x{:08X}. The current inference backend is unchanged.\n{}",
            exit_code, diagnostic_tail.empty() ? "The compiler produced no stdout/stderr diagnostics."
                                               : "Compiler output:\n" + diagnostic_tail));
    }
}

} // namespace llavon::service::ryzen_ai::detail
