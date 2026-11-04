#pragma once

#if defined(_WIN32)

#include "candidate_ui_snapshot.hpp"
#include "../candidate/candidate_ui.hpp"
#include "candidate_pipe_protocol.hpp"
#include "pipe_listener.hpp"

#include <asio.hpp>
#include <llavon-ipc/pipe_security.hpp>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>

namespace llavon::service {

class CandidatePipeServer final {
public:
    explicit CandidatePipeServer(candidate::CandidateUi& candidate_ui)
        : candidate_ui_(candidate_ui) {}

    asio::awaitable<void> listen() {
        auto executor = co_await asio::this_coro::executor;

        const auto pipe_name =
            llavon::ipc::session_pipe_name(candidate_pipe_protocol::pipe_base_name);
        ipc::PipeListener listener(pipe_name, PIPE_ACCESS_INBOUND, pipe_buffer_size);

        while (true) {
            try {
                auto pipe = co_await accept_pipe(listener);
                if (!pipe) co_return;
                const uint64_t client_id = next_client_id_++;
                asio::windows::stream_handle stream(executor);
                stream.assign(pipe.get());
                pipe.release();
                asio::co_spawn(
                    executor, handle_client(std::move(stream), client_id), asio::detached);
                continue;
            } catch (const std::exception& error) {
                std::cerr << "[IPC] candidate listener error; retrying: " << error.what() << '\n';
            } catch (...) {
                std::cerr << "[IPC] candidate listener error; retrying\n";
            }
            if (!co_await pipe_retry_delay()) co_return;
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
                candidate_ui_.present(candidate::Presentation{
                    .owner_window =
                        reinterpret_cast<HWND>(static_cast<ULONG_PTR>(snapshot.owner_window)),
                    .anchor_x = snapshot.anchor_x,
                    .anchor_y = snapshot.anchor_y,
                    .anchor_top = snapshot.anchor_top,
                    .candidates = std::move(snapshot.candidates),
                    .selection_index = snapshot.selection_index,
                    .layout_columns = snapshot.layout_columns,
                    .number_column = snapshot.number_column,
                    .can_prev_page = snapshot.can_prev_page,
                    .can_next_page = snapshot.can_next_page,
                });
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

    candidate::CandidateUi& candidate_ui_;
    uint64_t next_client_id_ = 1;
    uint64_t active_client_id_ = 0;
};

}  // namespace llavon::service

#endif
