#pragma once

#include <windows.h>
#include <llavon-ipc/pipe_security.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/bopomofo.hpp"
#include "engine.h"
#include "system/serviceLauncher.hpp"

namespace tsf {

enum class PipeCommand : uint8_t {
    Predict = 1,
    ToggleInputMode = 2,
    GetInputMode = 3,
    Ready = 4,
    RecordCommit = 5,
    DiscardLastCommit = 6,
    Prefill = 7,
};

class PipeEngine : public IEngine {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    static inline std::atomic_uint64_t next_commit_token{1};
    static constexpr const wchar_t* pipe_base_name = L"llavon-ime";
    static constexpr uint32_t maximum_candidate_count = 4096;
    static constexpr std::chrono::seconds launch_retry_interval{5};
    std::optional<std::chrono::steady_clock::time_point> last_launch_request;

    bool connect_pipe() {
        const auto pipe_name = llavon::ipc::session_pipe_name(pipe_base_name);
        hPipe = CreateFileW(pipe_name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (hPipe == INVALID_HANDLE_VALUE) return false;
        if (!llavon::ipc::is_trusted_pipe_server(hPipe)) {
            disconnect();
            return false;
        }
        return true;
    }

    // Starts the backend without waiting for its pipe. Until it is reachable the
    // engine reports English mode, so key handling never blocks while the backend
    // loads or while an update holds the launch mutex.
    void request_backend_launch() {
        const auto now = std::chrono::steady_clock::now();
        if (last_launch_request && now - *last_launch_request < launch_retry_interval) return;
        last_launch_request = now;

        HANDLE launch_mutex = CreateMutexW(nullptr, FALSE, service_launch_mutex_name);
        if (!launch_mutex) return;

        // The updater holds this mutex while it replaces the backend.
        const DWORD wait_result = WaitForSingleObject(launch_mutex, 0);
        if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED) {
            if (!connect_pipe()) {
                launch_service_backend();
            }
            ReleaseMutex(launch_mutex);
        }
        CloseHandle(launch_mutex);
    }

    bool ensure_pipe() {
        if (hPipe != INVALID_HANDLE_VALUE) return true;
        if (connect_pipe()) return true;

        request_backend_launch();
        return hPipe != INVALID_HANDLE_VALUE;
    }

    void disconnect() {
        if (hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
        }
    }

    template <typename T>
    bool write_exact(const T& val) {
        DWORD written = 0;
        return WriteFile(hPipe, &val, sizeof(T), &written, nullptr) && written == sizeof(T);
    }

    bool write_exact(const void* buf, DWORD size) {
        DWORD written = 0;
        while (written < size) {
            DWORD n = 0;
            if (!WriteFile(hPipe, static_cast<const char*>(buf) + written, size - written, &n, nullptr) || n == 0) return false;
            written += n;
        }
        return true;
    }

    template <typename T>
    bool read_exact(T& val) {
        DWORD total = 0;
        while (total < sizeof(T)) {
            DWORD n = 0;
            if (!ReadFile(hPipe, reinterpret_cast<char*>(&val) + total, sizeof(T) - total, &n, nullptr) || n == 0)
                return false;
            total += n;
        }
        return true;
    }

    bool read_exact(void* buf, DWORD size) {
        DWORD total = 0;
        while (total < size) {
            DWORD n = 0;
            if (!ReadFile(hPipe, static_cast<char*>(buf) + total, size - total, &n, nullptr) || n == 0) return false;
            total += n;
        }
        return true;
    }

    bool write_command(PipeCommand command) {
        const uint8_t raw_command = static_cast<uint8_t>(command);
        return write_exact(raw_command);
    }

    bool read_input_mode(InputMode& mode) {
        uint8_t raw_mode = 0;
        if (!read_exact(raw_mode)) return false;

        mode = raw_mode == static_cast<uint8_t>(InputMode::English) ? InputMode::English : InputMode::Chinese;
        return true;
    }

public:
    PipeEngine() = default;
    ~PipeEngine() override { disconnect(); }
    PipeEngine(const PipeEngine&) = delete;
    PipeEngine& operator=(const PipeEngine&) = delete;

    void ready() override {
        if (!ensure_pipe()) return;

        if (!write_command(PipeCommand::Ready)) { disconnect(); return; }

        uint8_t ok = 0;
        if (!read_exact(ok)) { disconnect(); return; }
    }

    void prefill(const std::u16string& context) override {
        if (!ensure_pipe()) return;

        if (!write_command(PipeCommand::Prefill)) { disconnect(); return; }

        const uint32_t ctx_len = static_cast<uint32_t>(context.size());
        if (!write_exact(ctx_len)) { disconnect(); return; }
        if (ctx_len > 0 && !write_exact(context.data(), ctx_len * sizeof(char16_t))) { disconnect(); return; }
    }

    void predict(const std::u16string& context, std::span<BopomofoPos> padding) override {
        for (auto& p : padding) {
            if (!p.is_predictable_by_engine()) {
                return;
            }
        }

        if (!ensure_pipe()) return;

        // --- request ---
        if (!write_command(PipeCommand::Predict)) { disconnect(); return; }

        uint32_t ctx_len = static_cast<uint32_t>(context.size());
        if (!write_exact(ctx_len)) { disconnect(); return; }
        if (ctx_len > 0 && !write_exact(context.data(), ctx_len * sizeof(char16_t))) { disconnect(); return; }

        uint32_t pad_cnt = static_cast<uint32_t>(padding.size());
        if (!write_exact(pad_cnt)) { disconnect(); return; }

        for (auto& p : padding) {
            uint8_t type = p.chosen ? 1 : 0;
            if (!write_exact(type)) { disconnect(); return; }

            if (p.chosen) {
                char32_t c = p.current32();
                if (!write_exact(c)) { disconnect(); return; }
            } else {
                auto bpmf = p.to_bopomofo_string();
                uint32_t len = static_cast<uint32_t>(bpmf.size());
                if (!write_exact(len)) { disconnect(); return; }
                if (len > 0 && !write_exact(bpmf.data(), len * sizeof(char16_t))) { disconnect(); return; }
            }
        }

        // --- response ---
        uint32_t resp_cnt;
        if (!read_exact(resp_cnt) || resp_cnt > padding.size()) { disconnect(); return; }

        for (uint32_t i = 0; i < resp_cnt && i < static_cast<uint32_t>(padding.size()); i++) {
            uint32_t cand_cnt;
            if (!read_exact(cand_cnt) || cand_cnt > maximum_candidate_count) { disconnect(); return; }

            std::vector<char32_t> cands(cand_cnt);
            if (cand_cnt > 0 && !read_exact(cands.data(), cand_cnt * sizeof(char32_t))) { disconnect(); return; }

            if (!padding[i].chosen) {
                if (cands.size() == 1) {
                    padding[i].choose_candidate(cands.front());
                } else {
                    padding[i].set_candaiates(std::move(cands));
                }
            }
            padding[i].predicted = true;
        }
    }

    std::uint64_t record_commit(const CommitSample& sample) override {
        if (sample.answer.empty() || sample.input.empty() || !ensure_pipe()) return 0;

        if (!write_command(PipeCommand::RecordCommit)) { disconnect(); return 0; }

        const std::uint64_t token = next_commit_token.fetch_add(
            1, std::memory_order_relaxed);
        if (token == 0 || !write_exact(token)) {
            disconnect();
            return 0;
        }

        const auto write_string = [this](std::u16string_view value) {
            const auto length = static_cast<std::uint32_t>(value.size());
            if (!write_exact(length)) return false;
            return length == 0 || write_exact(value.data(), length * sizeof(char16_t));
        };

        if (!write_string(sample.context) || !write_string(sample.answer)) {
            disconnect();
            return 0;
        }

        const auto count = static_cast<std::uint32_t>(sample.input.size());
        if (!write_exact(count)) { disconnect(); return 0; }
        for (const auto& entry : sample.input) {
            const std::uint8_t manually_selected = entry.manually_selected ? 1 : 0;
            if (!write_string(entry.reading) || !write_string(entry.output) ||
                !write_exact(manually_selected)) {
                disconnect();
                return 0;
            }
        }
        return token;
    }

    void discard_commit(std::uint64_t token) override {
        if (token == 0) return;
        if (!ensure_pipe()) return;
        if (!write_command(PipeCommand::DiscardLastCommit) ||
            !write_exact(token)) {
            disconnect();
        }
    }

    // Without a backend there is no Bopomofo engine, so both mode queries
    // fall back to English and keys pass through to the application.
    InputMode toggle_input_mode() override {
        if (!ensure_pipe()) return InputMode::English;

        if (!write_command(PipeCommand::ToggleInputMode)) { disconnect(); return InputMode::English; }

        InputMode mode = InputMode::English;
        if (!read_input_mode(mode)) { disconnect(); return InputMode::English; }
        return mode;
    }

    InputMode current_input_mode() override {
        if (!ensure_pipe()) return InputMode::English;

        if (!write_command(PipeCommand::GetInputMode)) { disconnect(); return InputMode::English; }

        InputMode mode = InputMode::English;
        if (!read_input_mode(mode)) { disconnect(); return InputMode::English; }
        return mode;
    }
};

}  // namespace tsf
