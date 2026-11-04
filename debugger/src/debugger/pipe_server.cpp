#include "pipe_server.hpp"

#include "../pipe_protocol.hpp"

#include <llavon-ipc/pipe_security.hpp>
#include <llavon-ipc/pipe_listener.hpp>

#include <cstdint>
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace llavon::debugger {

PipeServer::PipeServer(ConnectionCallback connection_callback, MessageCallback message_callback)
    : connection_callback_(std::move(connection_callback)),
      message_callback_(std::move(message_callback)) {
    stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!stop_event_) throw std::runtime_error("unable to create debugger stop event");
    try {
        accept_thread_ = std::thread([this] { accept_clients(); });
    } catch (...) {
        CloseHandle(stop_event_);
        stop_event_ = nullptr;
        throw;
    }
}

PipeServer::~PipeServer() {
    if (stop_event_) SetEvent(stop_event_);
    if (accept_thread_.joinable()) accept_thread_.join();
    for (auto& client : client_threads_) {
        if (client.thread.joinable()) client.thread.join();
    }
    if (stop_event_) CloseHandle(stop_event_);
}

void PipeServer::accept_clients() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    llavon::ipc::PipeListener listener(
        llavon::ipc::session_pipe_name(llavon::debug::pipe_protocol::pipe_base_name), PIPE_ACCESS_INBOUND);
    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
        reap_clients();
        if (listener.ensure() != ERROR_SUCCESS) {
            WaitForSingleObject(stop_event_, 250);
            continue;
        }

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            WaitForSingleObject(stop_event_, 250);
            continue;
        }
        bool connected = false;
        {
            llavon::ipc::PendingPipeConnection connection(listener.get(), event);
            const DWORD error = connection.start();
            if (error == ERROR_SUCCESS) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                HANDLE waits[] = {stop_event_, event};
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0 + 1) {
                    connected = connection.complete() == ERROR_SUCCESS;
                }
            }
        }
        CloseHandle(event);

        if (!connected || WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
            listener.disconnect();
            WaitForSingleObject(stop_event_, 10);
            continue;
        }
        auto accepted = listener.take_connected();
        while (!accepted && WaitForSingleObject(stop_event_, 250) != WAIT_OBJECT_0) {
            accepted = listener.take_connected();
        }
        if (!accepted || WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) break;
        const HANDLE pipe = accepted->get();
        try {
            auto complete = std::make_shared<std::atomic<bool>>(false);
            client_threads_.push_back(ClientThread{});
            client_threads_.back().complete = complete;
            client_threads_.back().thread = std::thread([this, pipe, complete] {
                handle_client(pipe);
                complete->store(true, std::memory_order_release);
            });
            accepted->release();
        } catch (...) {
            if (!client_threads_.empty() && !client_threads_.back().thread.joinable()) {
                client_threads_.pop_back();
            }
        }
    }
}

void PipeServer::reap_clients() noexcept {
    const auto first_finished = std::remove_if(
        client_threads_.begin(), client_threads_.end(), [](ClientThread& client) {
            if (!client.complete->load(std::memory_order_acquire)) return false;
            if (client.thread.joinable()) client.thread.join();
            return true;
        });
    client_threads_.erase(first_finished, client_threads_.end());
}

void PipeServer::handle_client(HANDLE pipe) noexcept {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) {
        CloseHandle(pipe);
        return;
    }
    try {
        connection_callback_(1);
        for (;;) {
            std::uint32_t length = 0;
            if (!read_exact(pipe, event, &length, sizeof(length)) ||
                length < sizeof(std::uint8_t) ||
                length > llavon::debug::pipe_protocol::maximum_message_size) break;
            std::uint8_t raw_information = 0;
            if (!read_exact(pipe, event, &raw_information, sizeof(raw_information))) break;
            std::string message(length - sizeof(raw_information), '\0');
            if (!message.empty() &&
                !read_exact(pipe, event, message.data(), message.size())) break;
            const auto information = llavon::debug::pipe_protocol::decode_information(raw_information);
            message_callback_(information, std::move(message));
        }
    } catch (...) {
    }
    try {
        connection_callback_(-1);
    } catch (...) {
    }
    CancelIoEx(pipe, nullptr);
    CloseHandle(event);
    CloseHandle(pipe);
}

bool PipeServer::read_exact(HANDLE pipe, HANDLE event, void* buffer, std::size_t size) noexcept {
    auto* cursor = static_cast<std::uint8_t*>(buffer);
    std::size_t remaining = size;
    while (remaining != 0) {
        ResetEvent(event);
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        DWORD transferred = 0;
        if (!ReadFile(pipe, cursor, static_cast<DWORD>(remaining), &transferred, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            HANDLE waits[] = {stop_event_, event};
            const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
            if (wait != WAIT_OBJECT_0 + 1) {
                CancelIoEx(pipe, &overlapped);
                GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                return false;
            }
            if (!GetOverlappedResult(pipe, &overlapped, &transferred, FALSE)) return false;
        }
        if (transferred == 0) return false;
        cursor += transferred;
        remaining -= transferred;
    }
    return true;
}

}  // namespace llavon::debugger
