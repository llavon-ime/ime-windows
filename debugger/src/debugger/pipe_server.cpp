#include "pipe_server.hpp"

#include "../pipe_protocol.hpp"

#include <sddl.h>

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
    while (WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
        reap_clients();
        HANDLE pipe = create_pipe();
        if (pipe == INVALID_HANDLE_VALUE) {
            WaitForSingleObject(stop_event_, 250);
            continue;
        }

        HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!event) {
            CloseHandle(pipe);
            return;
        }
        OVERLAPPED overlapped{};
        overlapped.hEvent = event;
        bool connected = false;
        if (ConnectNamedPipe(pipe, &overlapped)) {
            connected = true;
        } else {
            const DWORD error = GetLastError();
            if (error == ERROR_PIPE_CONNECTED) {
                connected = true;
            } else if (error == ERROR_IO_PENDING) {
                HANDLE waits[] = {stop_event_, event};
                const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0 + 1) {
                    DWORD transferred = 0;
                    connected = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
                } else {
                    CancelIoEx(pipe, &overlapped);
                    DWORD transferred = 0;
                    GetOverlappedResult(pipe, &overlapped, &transferred, TRUE);
                }
            }
        }
        CloseHandle(event);

        if (!connected || WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
            CloseHandle(pipe);
            continue;
        }
        try {
            auto complete = std::make_shared<std::atomic<bool>>(false);
            client_threads_.push_back(ClientThread{});
            client_threads_.back().complete = complete;
            client_threads_.back().thread = std::thread([this, pipe, complete] {
                handle_client(pipe);
                complete->store(true, std::memory_order_release);
            });
        } catch (...) {
            if (!client_threads_.empty() && !client_threads_.back().thread.joinable()) {
                client_threads_.pop_back();
            }
            CloseHandle(pipe);
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

HANDLE PipeServer::create_pipe() const noexcept {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    constexpr wchar_t sddl[] =
        L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl, SDDL_REVISION_1, &descriptor, nullptr)) return INVALID_HANDLE_VALUE;

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    HANDLE pipe = CreateNamedPipeW(
        llavon::debug::pipe_protocol::pipe_name,
        PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &attributes);
    LocalFree(descriptor);
    return pipe;
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
                length > llavon::debug::pipe_protocol::maximum_message_size) break;
            std::string message(length, '\0');
            if (length != 0 && !read_exact(pipe, event, message.data(), length)) break;
            message_callback_(std::move(message));
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
