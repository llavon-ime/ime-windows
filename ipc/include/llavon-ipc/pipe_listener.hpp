#pragma once

// Listener handoff follows Microsoft PowerToys' two_way_pipe_message_ipc.cpp.
// Copyright (c) Microsoft Corporation. See ipc/POWERTOYS-LICENSE.txt.

#include "pipe_security.hpp"

#include <expected>
#include <utility>

namespace llavon::ipc {

class PipeListener final {
public:
    using CreatePipe = decltype(&CreateNamedPipeW);
    using AcceptedPipe = std::expected<detail::UniqueHandle, DWORD>;

    explicit PipeListener(std::wstring name, DWORD access, DWORD buffer_size = 65536,
                          CreatePipe create_pipe = &CreateNamedPipeW) noexcept
        : name_(std::move(name)), access_(access), buffer_size_(buffer_size), create_pipe_(create_pipe) {}

    PipeListener(const PipeListener&) = delete;
    PipeListener& operator=(const PipeListener&) = delete;

    HANDLE get() const noexcept { return listener_.get(); }

    DWORD ensure() noexcept {
        if (listener_) return ERROR_SUCCESS;
        auto first = create_instance(true);
        if (!first) return first.error();
        listener_ = std::move(*first);
        return ERROR_SUCCESS;
    }

    // A disconnected/rejected client does not relinquish the pipe name.
    void disconnect() noexcept { DisconnectNamedPipe(listener_.get()); }

    // Create the successor before handing the accepted instance to its client
    // handler. On failure, retain the accepted handle and let the caller retry.
    AcceptedPipe take_connected() noexcept {
        if (!listener_) return std::unexpected(ERROR_INVALID_HANDLE);
        auto replacement = create_instance(false);
        if (!replacement) return std::unexpected(replacement.error());
        listener_.swap(*replacement);
        return std::move(*replacement);
    }

private:
    AcceptedPipe create_instance(bool first) noexcept {
        if (name_.empty()) return std::unexpected(ERROR_INVALID_NAME);
        PipeSecurity security;
        if (!security) return std::unexpected(security.error());
        const HANDLE pipe = create_pipe_(name_.c_str(), access_ | FILE_FLAG_OVERLAPPED |
            (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, buffer_size_, buffer_size_, 0, security.attributes());
        if (pipe == INVALID_HANDLE_VALUE) return std::unexpected(GetLastError());
        return detail::UniqueHandle(pipe);
    }

    std::wstring name_;
    DWORD access_;
    DWORD buffer_size_;
    CreatePipe create_pipe_;
    detail::UniqueHandle listener_;
};

// An outstanding ConnectNamedPipe must finish before its OVERLAPPED/event is
// destroyed, including when an awaitable is cancelled or its context stops.
class PendingPipeConnection final {
public:
    PendingPipeConnection(HANDLE pipe, HANDLE event) noexcept : pipe_(pipe) { overlapped_.hEvent = event; }
    ~PendingPipeConnection() {
        if (pending_) {
            CancelIoEx(pipe_, &overlapped_);
            DWORD transferred = 0;
            GetOverlappedResult(pipe_, &overlapped_, &transferred, TRUE);
        }
    }
    PendingPipeConnection(const PendingPipeConnection&) = delete;
    PendingPipeConnection& operator=(const PendingPipeConnection&) = delete;

    DWORD start() noexcept {
        if (ConnectNamedPipe(pipe_, &overlapped_)) return ERROR_SUCCESS;
        const DWORD error = GetLastError();
        pending_ = error == ERROR_IO_PENDING;
        return error == ERROR_PIPE_CONNECTED ? ERROR_SUCCESS : error;
    }

    DWORD complete() noexcept {
        DWORD transferred = 0;
        if (GetOverlappedResult(pipe_, &overlapped_, &transferred, FALSE)) {
            pending_ = false;
            return ERROR_SUCCESS;
        }
        const DWORD error = GetLastError();
        pending_ = error == ERROR_IO_INCOMPLETE;
        return error;
    }

private:
    HANDLE pipe_;
    OVERLAPPED overlapped_{};
    bool pending_ = false;
};

}  // namespace llavon::ipc
