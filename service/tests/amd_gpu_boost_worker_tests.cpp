#include "service/amd_gpu_boost_worker.hpp"

#include <windows.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace llavon::service;
using Result = GpuActivityLease::Result;

namespace {
constexpr auto state_environment = L"LLAVON_AMD_BOOST_TEST_STATE";
struct Counters {
    volatile LONG enables;
    volatile LONG releases;
    volatile LONG enable_result;
    volatile LONG release_failures;
    volatile LONG delay_ms;
    volatile LONG unsupported;
};
struct Close { void operator()(void* p) const noexcept { if (p) CloseHandle(p); } };
using Handle = std::unique_ptr<void, Close>;
struct Unmap { void operator()(Counters* p) const noexcept { if (p) UnmapViewOfFile(p); } };
using View = std::unique_ptr<Counters, Unmap>;
LONG read(volatile LONG& value) { return InterlockedCompareExchange(&value, 0, 0); }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

GpuActivityLease::Boost fake_driver(std::string_view) {
    std::array<wchar_t, 128> name{};
    require(GetEnvironmentVariableW(state_environment, name.data(), static_cast<DWORD>(name.size())) != 0,
            "missing test state");
    Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.data()));
    require(mapping != nullptr, "cannot open test state");
    View view(static_cast<Counters*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Counters))));
    require(view != nullptr, "cannot map test state");
    if (read(view->unsupported)) return {};
    return [mapping = std::move(mapping), view = std::move(view)](bool enabled) {
        if (enabled) {
            Sleep(static_cast<DWORD>(read(view->delay_ms)));
            InterlockedIncrement(&view->enables);
            return static_cast<Result>(read(view->enable_result));
        }
        InterlockedIncrement(&view->releases);
        if (read(view->release_failures) > 0) {
            InterlockedDecrement(&view->release_failures);
            return Result::retry_later;
        }
        return Result::success;
    };
}

void wait_count(volatile LONG& counter, LONG expected) {
    const auto deadline = GetTickCount64() + 6000;
    while (read(counter) < expected && GetTickCount64() < deadline) Sleep(10);
    require(read(counter) == expected, "unexpected driver call count");
}

void run_tests() {
    const auto name = L"Local\\LlavonAmdBoostTest-" + std::to_wstring(GetCurrentProcessId());
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Counters), name.c_str()));
    require(mapping != nullptr, "cannot create test state");
    View state(static_cast<Counters*>(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Counters))));
    require(state != nullptr, "cannot map test state");
    require(SetEnvironmentVariableW(state_environment, name.c_str()) != FALSE, "cannot set test state");
    // A non-existent hardware BDF is intentional: these tests never load a driver.
    constexpr std::string_view device = "0000:fe:1f.7";
    LONG enabled = 0;
    LONG released = 0;
    {
        auto boost = make_amd_boost_worker(device);
        require(read(state->enables) == 0 && read(state->releases) == 0, "initialization changed clocks");
        require(boost(true) == Result::success, "enable failed");
        ++enabled;
        require(boost(false) == Result::success, "release failed");
        ++released;
        // A second owner cannot race the first worker's restoration.
        bool rejected = false;
        try { (void)make_amd_boost_worker("0:FE:1F.7"); } catch (...) { rejected = true; }
        require(rejected, "duplicate device worker accepted");
    }
    wait_count(state->enables, enabled);
    wait_count(state->releases, released);
    {
        auto boost = make_amd_boost_worker(device);
        require(boost(true) == Result::success, "enable before disposal failed");
        ++enabled;
    }
    wait_count(state->releases, ++released);
    {
        auto boost = make_amd_boost_worker(device);
        InterlockedExchange(&state->enable_result, static_cast<LONG>(Result::retry_later));
        require(boost(true) == Result::retry_later, "transient failure lost");
        ++enabled;
        wait_count(state->releases, ++released);
        InterlockedExchange(&state->enable_result, static_cast<LONG>(Result::success));
        require(boost(true) == Result::success, "retry failed");
        ++enabled;
        InterlockedExchange(&state->release_failures, 2);
        require(boost(false) == Result::retry_later, "release failure lost");
        ++released;
    }
    // The worker retries after its owner has disposed the backend.
    wait_count(state->releases, released += 2);
    Sleep(50); // Allow process teardown and mutex release after the final call.
    {
        auto boost = make_amd_boost_worker(device);
        InterlockedExchange(&state->delay_ms, 1500);
        require(boost(true) == Result::unavailable, "hung call did not disconnect");
        require(boost(true) == Result::unavailable, "disconnected backend accepted new work");
        ++enabled;
    }
    wait_count(state->releases, ++released);
    InterlockedExchange(&state->delay_ms, 0);
    Sleep(50);
    InterlockedExchange(&state->unsupported, 1);
    bool rejected = false;
    try { (void)make_amd_boost_worker(device); } catch (...) { rejected = true; }
    require(rejected, "unsupported driver accepted");
    InterlockedExchange(&state->unsupported, 0);
    Sleep(50);

    // Kill only our test parent. Its independent worker must survive and restore.
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    require(length != 0 && length < executable.size(), "executable path failed");
    executable.resize(length);
    std::wstring command = L"\"" + executable + L"\" --crash-parent";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "test parent launch failed");
    Handle child(process.hProcess);
    const Handle thread(process.hThread);
    // Ensure the child is also cleaned up if a test assertion throws.
    struct KillChild { HANDLE child; ~KillChild() { TerminateProcess(child, 90); } } cleanup{child.get()};
    wait_count(state->enables, ++enabled);
    require(TerminateProcess(child.get(), 91) != FALSE, "test parent termination failed");
    wait_count(state->releases, ++released);
    require(read(state->enables) == enabled, "unexpected extra enable");
    require(SetEnvironmentVariableW(state_environment, nullptr) != FALSE, "test state cleanup failed");
    std::cout << "AMD worker: initialization, exclusion, release, retry, timeout, unsupported, parent crash passed\n";
}
} // namespace

int main(int argc, char** argv) {
    if (const auto worker = run_amd_boost_worker(argc, argv, fake_driver)) return *worker;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--crash-parent") {
            auto boost = make_amd_boost_worker("0000:fe:1f.7");
            require(boost(true) == Result::success, "crash test enable failed");
            Sleep(INFINITE);
            return 1;
        }
        run_tests();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
