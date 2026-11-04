#pragma once

#include <asio.hpp>
#include <llavon-ipc/pipe_listener.hpp>

#include <chrono>
#include <iostream>

namespace llavon::service {

inline asio::awaitable<bool> pipe_retry_delay() {
    asio::steady_timer delay(co_await asio::this_coro::executor, std::chrono::milliseconds(250));
    const auto [error] = co_await delay.async_wait(asio::as_tuple(asio::use_awaitable));
    co_return !error;
}

inline asio::awaitable<ipc::detail::UniqueHandle> accept_pipe(ipc::PipeListener& listener) {
    auto executor = co_await asio::this_coro::executor;
    while (true) {
        DWORD error = listener.ensure();
        if (error == ERROR_SUCCESS) {
            const HANDLE raw_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!raw_event) {
                error = GetLastError();
            } else {
                ipc::detail::UniqueHandle event_guard(raw_event);
                asio::windows::object_handle event(executor);
                event.assign(event_guard.get());
                event_guard.release();
                ipc::PendingPipeConnection connection(listener.get(), event.native_handle());
                error = connection.start();
                if (error == ERROR_IO_PENDING) {
                    const auto [wait_error] = co_await event.async_wait(asio::as_tuple(asio::use_awaitable));
                    if (wait_error) co_return ipc::detail::UniqueHandle{};
                    error = connection.complete();
                }
            }
            if (error != ERROR_SUCCESS) listener.disconnect();
        }
        if (error != ERROR_SUCCESS) {
            std::cerr << "[IPC] accept failed; retrying: " << error << '\n';
            if (!co_await pipe_retry_delay()) co_return ipc::detail::UniqueHandle{};
            continue;
        }
        while (true) {
            auto accepted = listener.take_connected();
            if (accepted) co_return std::move(*accepted);
            std::cerr << "[IPC] replacement listener failed; retaining pipe and retrying: "
                      << accepted.error() << '\n';
            if (!co_await pipe_retry_delay()) co_return ipc::detail::UniqueHandle{};
        }
    }
}

}  // namespace llavon::service
