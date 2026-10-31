#pragma once

#include <windows.h>
#include <llavon-ipc/pipe_security.hpp>
#include <winrt/Windows.Foundation.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "core/bopomofo.hpp"
#include "engine.h"
#include "system/serviceLauncher.hpp"

namespace tsf {

enum class PipeCommand : std::uint8_t {
    Predict = 1,
    ToggleInputMode = 2,
    GetInputMode = 3,
    Ready = 4,
    RecordCommit = 5,
    DiscardLastCommit = 6,
    Prefill = 7,
};

class PipeEngine final : public IEngine {
public:
    PipeEngine() = default;
    ~PipeEngine() override { disconnect(); }
    PipeEngine(const PipeEngine&) = delete;
    PipeEngine& operator=(const PipeEngine&) = delete;

    IAsyncAction ready() override {
        if (!ensure_pipe()) co_return;
        std::uint8_t ok = 0;
        if (!co_await send(Request(PipeCommand::Ready)) || !co_await receive(ok)) disconnect();
    }

    IAsyncAction prefill(std::u16string context) override {
        if (!ensure_pipe()) co_return;
        Request request(PipeCommand::Prefill);
        request.string(context);
        if (!co_await send(request)) disconnect();
    }

    IAsyncOperation<bool> predict(std::u16string context, std::vector<BopomofoPos>& padding) override {
        if (!std::ranges::all_of(padding, &BopomofoPos::is_predictable_by_engine)) co_return false;
        if (!ensure_pipe()) co_return false;

        Request request(PipeCommand::Predict);
        request.string(context);
        request.value(static_cast<std::uint32_t>(padding.size()));
        for (const auto& entry : padding) {
            request.value(static_cast<std::uint8_t>(entry.chosen ? 1 : 0));
            if (entry.chosen) {
                request.value(entry.current32());
            } else {
                request.string(entry.to_bopomofo_string());
            }
        }
        if (!co_await send(request)) { disconnect(); co_return false; }

        std::uint32_t count = 0;
        if (!co_await receive(count) || count > padding.size()) { disconnect(); co_return false; }
        for (std::uint32_t index = 0; index < count; ++index) {
            std::uint32_t candidate_count = 0;
            if (!co_await receive(candidate_count) || candidate_count > maximum_candidate_count) {
                disconnect();
                co_return false;
            }
            std::vector<char32_t> candidates(candidate_count);
            if (!co_await transfer(false, candidates.data(), candidate_count * sizeof(char32_t))) {
                disconnect();
                co_return false;
            }
            auto& entry = padding[index];
            if (!entry.chosen) {
                if (candidates.size() == 1) {
                    entry.choose_candidate(candidates.front());
                } else {
                    entry.set_candaiates(std::move(candidates));
                }
            }
            entry.predicted = true;
        }
        co_return true;
    }

    IAsyncOperation<std::uint64_t> record_commit(CommitSample sample) override {
        if (sample.answer.empty() || sample.input.empty() || !ensure_pipe()) co_return 0;

        const std::uint64_t token = next_commit_token.fetch_add(1, std::memory_order_relaxed);
        Request request(PipeCommand::RecordCommit);
        request.value(token);
        request.string(sample.context);
        request.string(sample.answer);
        request.value(static_cast<std::uint32_t>(sample.input.size()));
        for (const auto& entry : sample.input) {
            request.string(entry.reading);
            request.string(entry.output);
            request.value(static_cast<std::uint8_t>(entry.manually_selected ? 1 : 0));
        }
        if (!co_await send(request)) { disconnect(); co_return 0; }
        co_return token;
    }

    IAsyncAction discard_commit(std::uint64_t token) override {
        if (token == 0 || !ensure_pipe()) co_return;
        Request request(PipeCommand::DiscardLastCommit);
        request.value(token);
        if (!co_await send(request)) disconnect();
    }

    // Without a backend there is no Bopomofo engine, so both mode queries
    // fall back to English and keys pass through to the application.
    IAsyncOperation<std::uint8_t> toggle_input_mode() override {
        return query_input_mode(PipeCommand::ToggleInputMode);
    }

    IAsyncOperation<std::uint8_t> current_input_mode() override {
        return query_input_mode(PipeCommand::GetInputMode);
    }

    void abort() noexcept override {
        aborted_ = true;
        std::lock_guard lock(handle_mutex_);
        if (pipe_ != INVALID_HANDLE_VALUE) CancelIoEx(pipe_, nullptr);
    }

private:
    class Request {
    public:
        explicit Request(PipeCommand command) { value(static_cast<std::uint8_t>(command)); }

        template <typename T>
            requires std::is_trivially_copyable_v<T>
        void value(const T& data) {
            const auto* bytes = reinterpret_cast<const std::byte*>(&data);
            bytes_.insert(bytes_.end(), bytes, bytes + sizeof(T));
        }

        void string(std::u16string_view text) {
            value(static_cast<std::uint32_t>(text.size()));
            const auto* bytes = reinterpret_cast<const std::byte*>(text.data());
            bytes_.insert(bytes_.end(), bytes, bytes + text.size() * sizeof(char16_t));
        }

        std::byte* data() { return bytes_.data(); }
        std::size_t size() const { return bytes_.size(); }

    private:
        std::vector<std::byte> bytes_;
    };

    static inline std::atomic_uint64_t next_commit_token{1};
    static constexpr const wchar_t* pipe_base_name = L"llavon-ime";
    static constexpr std::uint32_t maximum_candidate_count = 4096;
    static constexpr std::chrono::seconds launch_retry_interval{5};

    std::mutex handle_mutex_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    winrt::handle io_event_{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    std::atomic_bool aborted_ = false;
    std::optional<std::chrono::steady_clock::time_point> last_launch_request_;

    bool ensure_pipe() {
        if (aborted_) return false;
        if (pipe_ != INVALID_HANDLE_VALUE) return true;
        if (connect_pipe()) return true;
        request_backend_launch();
        return pipe_ != INVALID_HANDLE_VALUE;
    }

    bool connect_pipe() {
        const auto pipe_name = llavon::ipc::session_pipe_name(pipe_base_name);
        const HANDLE handle = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                          OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        if (!llavon::ipc::is_trusted_pipe_server(handle)) {
            CloseHandle(handle);
            return false;
        }
        std::lock_guard lock(handle_mutex_);
        pipe_ = handle;
        return true;
    }

    // Starts the backend without waiting for its pipe. Until it is reachable the
    // engine reports English mode, so key handling never waits while the backend
    // loads or while an update holds the launch mutex.
    void request_backend_launch() {
        const auto now = std::chrono::steady_clock::now();
        if (last_launch_request_ && now - *last_launch_request_ < launch_retry_interval) return;
        last_launch_request_ = now;

        const HANDLE launch_mutex = CreateMutexW(nullptr, FALSE, service_launch_mutex_name);
        if (!launch_mutex) return;

        // The updater holds this mutex while it replaces the backend.
        const DWORD wait_result = WaitForSingleObject(launch_mutex, 0);
        if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
            if (!connect_pipe()) launch_service_backend();
            ReleaseMutex(launch_mutex);
        }
        CloseHandle(launch_mutex);
    }

    void disconnect() noexcept {
        std::lock_guard lock(handle_mutex_);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    // One overlapped read or write of exactly size bytes. Completes when the
    // backend answers or its pipe breaks; abort() cancels it on shutdown.
    IAsyncOperation<bool> transfer(bool write, void* data, std::size_t size) {
        auto* bytes = static_cast<std::byte*>(data);
        std::size_t done = 0;
        while (done < size) {
            OVERLAPPED overlapped{};
            overlapped.hEvent = io_event_.get();
            ResetEvent(overlapped.hEvent);
            const auto chunk = static_cast<DWORD>(std::min<std::size_t>(size - done, MAXDWORD));
            const BOOL started = write ? WriteFile(pipe_, bytes + done, chunk, nullptr, &overlapped)
                                       : ReadFile(pipe_, bytes + done, chunk, nullptr, &overlapped);
            if (!started && GetLastError() != ERROR_IO_PENDING) co_return false;
            // abort() sets the flag before cancelling, so an operation started
            // after its CancelIoEx is still cancelled here.
            if (aborted_) CancelIoEx(pipe_, &overlapped);
            co_await winrt::resume_on_signal(overlapped.hEvent);
            DWORD transferred = 0;
            if (!GetOverlappedResult(pipe_, &overlapped, &transferred, FALSE) || transferred == 0) {
                co_return false;
            }
            done += transferred;
        }
        co_return true;
    }

    IAsyncOperation<bool> send(Request request) {
        co_return co_await transfer(true, request.data(), request.size());
    }

    template <typename T>
        requires std::is_trivially_copyable_v<T>
    IAsyncOperation<bool> receive(T& value) {
        return transfer(false, &value, sizeof(T));
    }

    IAsyncOperation<std::uint8_t> query_input_mode(PipeCommand command) {
        constexpr auto english = static_cast<std::uint8_t>(InputMode::English);
        if (!ensure_pipe()) co_return english;
        std::uint8_t raw_mode = 0;
        if (!co_await send(Request(command)) || !co_await receive(raw_mode)) {
            disconnect();
            co_return english;
        }
        co_return raw_mode == english ? english : static_cast<std::uint8_t>(InputMode::Chinese);
    }
};

}  // namespace tsf
