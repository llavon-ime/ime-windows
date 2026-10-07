#include "service/lora_process.hpp"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct CleanupProcess {
    void operator()(void* process) const noexcept {
        if (!process) return;
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 99);
            WaitForSingleObject(process, 5000);
        }
        CloseHandle(process);
    }
};
using Process = std::unique_ptr<void, CleanupProcess>;

std::filesystem::path executable() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    require(size != 0 && size < path.size(), "executable path unavailable");
    path.resize(size);
    return path;
}

PROCESS_INFORMATION spawn(std::wstring arguments, bool inherit = false) {
    auto command = L"\"" + executable().wstring() + L"\" " + arguments;
    STARTUPINFOW startup{sizeof(startup)};
    if (inherit) {
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    }
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable().c_str(), command.data(), nullptr, nullptr,
                          inherit, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "test process creation failed");
    CloseHandle(process.hThread);
    return process;
}

void write_output(std::string_view text, DWORD stream = STD_OUTPUT_HANDLE) {
    DWORD written = 0;
    require(WriteFile(GetStdHandle(stream), text.data(), static_cast<DWORD>(text.size()),
                      &written, nullptr) != FALSE && written == text.size(), "output failed");
}

std::vector<Process> open_tree(std::string_view line) {
    require(line.starts_with("tree="), "missing process tree");
    std::istringstream input{std::string(line.substr(5))};
    DWORD parent = 0;
    DWORD child = 0;
    input >> parent >> child;
    require(parent != 0 && child != 0, "invalid process tree");
    std::vector<Process> result;
    for (const auto id : {parent, child}) {
        Process process(OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, id));
        require(process != nullptr, "cannot inspect test child");
        result.push_back(std::move(process));
    }
    return result;
}

void require_stopped(const std::vector<Process>& tree) {
    for (const auto& process : tree)
        require(WaitForSingleObject(process.get(), 3000) == WAIT_OBJECT_0, "LoRA process escaped cleanup");
}

void run_tests() {
    using llavon::service::run_lora_process;
    std::atomic_bool cancelling{false};
    std::vector<std::string> lines;
    const auto exit_code = run_lora_process(executable(), {L"--emit"}, cancelling,
        [&](std::string_view line) { lines.emplace_back(line); });
    require(exit_code == 7, "exit status lost");
    require(lines == std::vector<std::string>{"loading", "validating=1/2 epoch=0",
        "step=1/2 epoch=1", "last error"}, "stream framing or final stderr line lost");

    // Receiving output must not depend on process completion. Each callback
    // runs while the child is still alive and initiates termination itself.
    for (const bool callback_throws : {false, true}) {
        cancelling = false;
        std::vector<Process> tree;
        bool failed = false;
        try {
            run_lora_process(executable(), {L"--tree"}, cancelling, [&](std::string_view line) {
                tree = open_tree(line);
                if (callback_throws) throw std::runtime_error("callback failure");
                cancelling = true;
            });
        } catch (const std::runtime_error& error) {
            failed = std::string_view(error.what()) ==
                (callback_throws ? "callback failure" : "operation cancelled");
        }
        require(failed && tree.size() == 2, "live output or cancellation failed");
        require_stopped(tree);
    }

    cancelling = false;
    std::vector<Process> descendants;
    require(run_lora_process(executable(), {L"--tree-exit"}, cancelling,
        [&](std::string_view line) { descendants = open_tree(line); }) == 0,
        "normal child exit failed");
    require_stopped(descendants);

    cancelling = false;
    {
        std::jthread cancel_thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            cancelling = true;
        });
        bool cancelled = false;
        try {
            run_lora_process(executable(), {L"--hang"}, cancelling, [](auto) {});
        } catch (const std::runtime_error& error) {
            cancelled = std::string_view(error.what()) == "operation cancelled";
        }
        require(cancelled, "silent child cannot be cancelled");
    }
    bool cancelled = false;
    try {
        run_lora_process(executable(), {L"--hang"}, cancelling, [](auto) {});
    } catch (const std::runtime_error& error) {
        cancelled = std::string_view(error.what()) == "operation cancelled";
    }
    require(cancelled, "cancellation before launch ignored");

    // Simulate the service being killed: its destructors never run.
    const auto marker = std::filesystem::temp_directory_path() /
        (L"llavon-lora-process-test-" + std::to_wstring(GetCurrentProcessId()) + L".txt");
    struct RemoveMarker {
        std::filesystem::path path;
        ~RemoveMarker() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{marker};
    Process owner(spawn(L"--owner \"" + marker.wstring() + L"\"").hProcess);
    std::string tree_line;
    const auto deadline = GetTickCount64() + 5000;
    while (tree_line.empty() && GetTickCount64() < deadline) {
        std::ifstream input(marker);
        std::string candidate;
        std::getline(input, candidate);
        if (input && !candidate.empty()) tree_line = candidate;
        if (tree_line.empty()) Sleep(10);
    }
    auto tree = open_tree(tree_line);
    require(TerminateProcess(owner.get(), 42) != FALSE, "cannot simulate service exit");
    require(WaitForSingleObject(owner.get(), 3000) == WAIT_OBJECT_0, "owner did not exit");
    require_stopped(tree);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc > 1) {
            const std::wstring_view mode(argv[1]);
            if (mode == L"--hang") { Sleep(INFINITE); return 0; }
            if (mode == L"--emit") {
                write_output("loading\r\nvalidating=1/2 epoch=0\rstep=");
                Sleep(100);
                write_output("1/2 epoch=1\n");
                write_output("last error", STD_ERROR_HANDLE);
                return 7;
            }
            if (mode == L"--tree" || mode == L"--tree-exit") {
                const auto child = spawn(L"--hang", true);
                CloseHandle(child.hProcess);
                write_output("tree=" + std::to_string(GetCurrentProcessId()) + " " +
                             std::to_string(child.dwProcessId) + "\n");
                Sleep(mode == L"--tree" ? INFINITE : 300);
                return 0;
            }
            if (mode == L"--owner" && argc == 3) {
                std::atomic_bool cancelling{false};
                return static_cast<int>(llavon::service::run_lora_process(
                    executable(), {L"--tree"}, cancelling, [&](std::string_view line) {
                        std::ofstream output{std::filesystem::path(argv[2])};
                        output << line << '\n';
                    }));
            }
            return 2;
        }
        run_tests();
        std::cout << "LoRA process lifecycle tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
