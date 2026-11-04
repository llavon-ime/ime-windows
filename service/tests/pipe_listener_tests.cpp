#include "service/pipe_listener.hpp"

#include <atomic>
#include <cstdio>
#include <future>
#include <stdexcept>
#include <thread>

namespace {
using llavon::ipc::detail::UniqueHandle;
using namespace std::chrono_literals;

void require(bool success, const char* operation) {
    if (!success) throw std::runtime_error(std::format("{}: {}", operation, GetLastError()));
}

std::atomic_uint create_calls = 0;
std::atomic_uint injected_failures = 0;
HANDLE WINAPI flaky_create(LPCWSTR name, DWORD access, DWORD mode, DWORD instances,
                          DWORD out_size, DWORD in_size, DWORD timeout, LPSECURITY_ATTRIBUTES security) {
    const auto call = ++create_calls;
    if (call == 1 || call == 2 || call == 4 || call == 5) {
        ++injected_failures;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return INVALID_HANDLE_VALUE;
    }
    return CreateNamedPipeW(name, access, mode, instances, out_size, in_size, timeout, security);
}

class IoRunner final {
public:
    explicit IoRunner(asio::io_context& io) : io_(io), worker_([&io] { io.run(); }) {}
    ~IoRunner() {
        io_.stop();
        worker_.join();
    }
    IoRunner(const IoRunner&) = delete;
    IoRunner& operator=(const IoRunner&) = delete;
private:
    asio::io_context& io_;
    std::thread worker_;
};

UniqueHandle connect_client(const std::wstring& name) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        const HANDLE raw = CreateFileW(name.c_str(), llavon::ipc::pipe_client_read_write_access,
            0, nullptr, OPEN_EXISTING, llavon::ipc::pipe_client_security_flags, nullptr);
        if (raw != INVALID_HANDLE_VALUE) return UniqueHandle(raw);
        const DWORD error = GetLastError();
        require(error == ERROR_FILE_NOT_FOUND || error == ERROR_PIPE_BUSY, "retry only unavailable/busy pipe");
        std::this_thread::sleep_for(10ms);
    }
    require(false, "client connection timeout");
    return {};
}

asio::awaitable<void> echo_clients(llavon::ipc::PipeListener& listener, unsigned count) {
    auto executor = co_await asio::this_coro::executor;
    unsigned served = 0;
    while (served < count) {
        auto accepted = co_await llavon::service::accept_pipe(listener);
        require(static_cast<bool>(accepted), "listener unexpectedly cancelled");
        asio::windows::stream_handle stream(executor);
        stream.assign(accepted.get());
        accepted.release();
        char byte = 0;
        const auto [read_error, read] = co_await asio::async_read(stream, asio::buffer(&byte, 1),
            asio::as_tuple(asio::use_awaitable));
        if (read_error) continue;  // An app can close immediately after opening the pipe.
        require(read == 1 && byte == 'x', "read client input");
        const auto [write_error, written] = co_await asio::async_write(stream, asio::buffer(&byte, 1),
            asio::as_tuple(asio::use_awaitable));
        require(!write_error && written == 1, "write server response");
        // Keep the server endpoint alive until the client has consumed its reply.
        const auto [ack_error, acknowledged] = co_await asio::async_read(stream, asio::buffer(&byte, 1),
            asio::as_tuple(asio::use_awaitable));
        require(!ack_error && acknowledged == 1 && byte == 'x', "client consumed response");
        ++served;
    }
}

void roundtrip(HANDLE client) {
    constexpr char sent = 'x';
    DWORD written = 0;
    require(WriteFile(client, &sent, 1, &written, nullptr) && written == 1, "write roundtrip byte");
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        DWORD available = 0;
        require(PeekNamedPipe(client, nullptr, 0, nullptr, &available, nullptr) != FALSE,
                "server stays alive until response");
        if (available) {
            char received = 0;
            DWORD read = 0;
            require(ReadFile(client, &received, 1, &read, nullptr) && read == 1 && received == sent,
                    "complete roundtrip");
            require(WriteFile(client, &sent, 1, &written, nullptr) && written == 1,
                    "acknowledge response");
            return;
        }
        std::this_thread::sleep_for(1ms);
    }
    require(false, "roundtrip timeout");
}
}  // namespace

int main() try {
    const auto name = std::format(L"\\\\.\\pipe\\llavon-async-listener-test-{}", GetCurrentProcessId());
    llavon::ipc::PipeListener listener(name, PIPE_ACCESS_DUPLEX, 4096, &flaky_create);
    // The context must be destroyed before listener, so cancelled coroutine frames
    // drain their pending ConnectNamedPipe while its handle is still valid.
    asio::io_context io;
    auto finished = asio::co_spawn(io, echo_clients(listener, 5), asio::use_future);
    asio::steady_timer watchdog(io, 10s);
    watchdog.async_wait([&io](const asio::error_code&) { io.stop(); });
    const IoRunner runner(io);

    for (unsigned index = 0; index < 3; ++index) {
        auto abandoned = connect_client(name);
        abandoned.reset();
    }
    for (unsigned index = 0; index < 5; ++index) {
        const auto client = connect_client(name);
        roundtrip(client.get());
    }
    require(finished.wait_for(5s) == std::future_status::ready, "service listener completion");
    finished.get();
    require(injected_failures == 4, "all initial/replacement failures were exercised");
    std::puts("PASS: service accept loop retries 4 injected failures, survives abandoned clients, serves 5 requests");
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
