#pragma once

#if defined(_WIN32)

#include "candidate_ui_loader.hpp"
#include "candidate_pipe_protocol.hpp"

#include <asio.hpp>
#include <sddl.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace llavon::service {

class CandidatePipeServer final {
public:
    explicit CandidatePipeServer(CandidateUiLoader& candidate_ui) : candidate_ui_(candidate_ui) {}

    asio::awaitable<void> listen() {
        auto executor = co_await asio::this_coro::executor;

        while (true) {
            PSECURITY_DESCRIPTOR security_descriptor = nullptr;
            constexpr wchar_t pipe_sddl[] =
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)(A;;GRGW;;;AC)(A;;GRGW;;;S-1-15-2-2)";
            if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                    pipe_sddl, SDDL_REVISION_1, &security_descriptor, nullptr)) {
                std::cerr << "[ERR] candidate pipe security descriptor failed: "
                          << GetLastError() << std::endl;
                co_return;
            }

            SECURITY_ATTRIBUTES security_attributes{};
            security_attributes.nLength = sizeof(security_attributes);
            security_attributes.lpSecurityDescriptor = security_descriptor;
            HANDLE pipe_handle = CreateNamedPipeW(
                candidate_pipe_protocol::pipe_name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                pipe_buffer_size, pipe_buffer_size, 0, &security_attributes);
            LocalFree(security_descriptor);
            if (pipe_handle == INVALID_HANDLE_VALUE) {
                std::cerr << "[ERR] CreateNamedPipe(candidate) failed: " << GetLastError()
                          << std::endl;
                co_return;
            }

            OVERLAPPED overlapped{};
            overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!overlapped.hEvent) {
                CloseHandle(pipe_handle);
                continue;
            }

            bool accepted = false;
            const BOOL connected = ConnectNamedPipe(pipe_handle, &overlapped);
            if (connected) {
                accepted = true;
                CloseHandle(overlapped.hEvent);
            } else {
                const DWORD error = GetLastError();
                if (error == ERROR_PIPE_CONNECTED) {
                    accepted = true;
                    CloseHandle(overlapped.hEvent);
                } else if (error == ERROR_IO_PENDING) {
                    asio::windows::object_handle event(executor, overlapped.hEvent);
                    const auto [wait_error] = co_await event.async_wait(
                        asio::as_tuple(asio::use_awaitable));
                    DWORD transferred = 0;
                    accepted = !wait_error &&
                               GetOverlappedResult(pipe_handle, &overlapped, &transferred, FALSE);
                } else {
                    CloseHandle(overlapped.hEvent);
                }
            }

            if (!accepted) {
                CloseHandle(pipe_handle);
                continue;
            }

            const uint64_t client_id = next_client_id_++;
            asio::windows::stream_handle stream(executor, pipe_handle);
            asio::co_spawn(
                executor, handle_client(std::move(stream), client_id), asio::detached);
        }
    }

private:
    static constexpr uint32_t pipe_buffer_size = 65536;

    static asio::awaitable<bool> read_exact(
        asio::windows::stream_handle& pipe, void* buffer, std::size_t size) {
        std::size_t total = 0;
        while (total < size) {
            auto [error, transferred] = co_await pipe.async_read_some(
                asio::buffer(static_cast<char*>(buffer) + total, size - total),
                asio::as_tuple(asio::use_awaitable));
            if (error || transferred == 0) {
                co_return false;
            }
            total += transferred;
        }
        co_return true;
    }

    template <typename Value>
    static asio::awaitable<bool> read_value(
        asio::windows::stream_handle& pipe, Value& value) {
        co_return co_await read_exact(pipe, &value, sizeof(value));
    }

    static asio::awaitable<bool> read_presentation(
        asio::windows::stream_handle& pipe, CandidateUiSnapshot& snapshot) {
        uint32_t candidate_count = 0;
        uint8_t can_prev_page = 0;
        uint8_t can_next_page = 0;
        if (!co_await read_value(pipe, snapshot.owner_window) ||
            !co_await read_value(pipe, snapshot.anchor_x) ||
            !co_await read_value(pipe, snapshot.anchor_y) ||
            !co_await read_value(pipe, snapshot.anchor_top) ||
            !co_await read_value(pipe, candidate_count) ||
            !co_await read_value(pipe, snapshot.selection_index) ||
            !co_await read_value(pipe, snapshot.layout_columns) ||
            !co_await read_value(pipe, snapshot.number_column) ||
            !co_await read_value(pipe, can_prev_page) ||
            !co_await read_value(pipe, can_next_page)) {
            co_return false;
        }

        if (!candidate_pipe_protocol::valid_presentation_header(
                candidate_count, snapshot.selection_index, snapshot.layout_columns,
                snapshot.number_column, can_prev_page, can_next_page)) {
            co_return false;
        }

        snapshot.can_prev_page = can_prev_page != 0;
        snapshot.can_next_page = can_next_page != 0;
        snapshot.candidates.clear();
        snapshot.candidates.reserve(candidate_count);
        for (uint32_t index = 0; index < candidate_count; ++index) {
            uint32_t length = 0;
            if (!co_await read_value(pipe, length) ||
                length > candidate_pipe_protocol::maximum_candidate_length) {
                co_return false;
            }
            std::wstring value(length, L'\0');
            if (length != 0 &&
                !co_await read_exact(pipe, value.data(), length * sizeof(wchar_t))) {
                co_return false;
            }
            snapshot.candidates.push_back(std::move(value));
        }
        co_return true;
    }

    asio::awaitable<void> handle_client(
        asio::windows::stream_handle pipe, uint64_t client_id) {
        while (true) {
            uint8_t raw_command = 0;
            uint16_t version = 0;
            if (!co_await read_value(pipe, raw_command) ||
                !co_await read_value(pipe, version) ||
                version != candidate_pipe_protocol::protocol_version) {
                break;
            }

            const auto command = static_cast<candidate_pipe_protocol::Command>(raw_command);
            if (command == candidate_pipe_protocol::Command::Present) {
                CandidateUiSnapshot snapshot;
                if (!co_await read_presentation(pipe, snapshot)) {
                    break;
                }
                active_client_id_ = client_id;
                candidate_ui_.present(snapshot);
                continue;
            }
            if (command == candidate_pipe_protocol::Command::Hide) {
                hide_if_active(client_id);
                continue;
            }
            break;
        }

        hide_if_active(client_id);
    }

    void hide_if_active(uint64_t client_id) {
        if (active_client_id_ != client_id) {
            return;
        }
        active_client_id_ = 0;
        candidate_ui_.hide();
    }

    CandidateUiLoader& candidate_ui_;
    uint64_t next_client_id_ = 1;
    uint64_t active_client_id_ = 0;
};

}  // namespace llavon::service

#endif
