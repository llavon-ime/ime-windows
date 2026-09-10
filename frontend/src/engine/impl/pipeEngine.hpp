#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "core/bopomofo.hpp"
#include "engine.h"
#include "system/globals.h"

namespace tsf {

enum class PipeCommand : uint8_t {
    Predict = 1,
    ToggleInputMode = 2,
    GetInputMode = 3,
    Ready = 4,
};

class PipeEngine : public IEngine {
    static inline HANDLE hPipe = INVALID_HANDLE_VALUE;
    static constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime";
    static constexpr const wchar_t* launch_mutex_name = L"Local\\LlavonImeBackendStart";

    static std::optional<std::filesystem::path> dll_dir() {
        if (!tsf::Globals::hinstance) {
            return std::nullopt;
        }

        std::wstring buffer(MAX_PATH, L'\0');
        for (;;) {
            const DWORD copied =
                GetModuleFileNameW(tsf::Globals::hinstance, buffer.data(), static_cast<DWORD>(buffer.size()));
            if (copied == 0) {
                return std::nullopt;
            }

            if (copied < static_cast<DWORD>(buffer.size() - 1)) {
                buffer.resize(copied);
                return std::filesystem::path(buffer).parent_path();
            }

            buffer.resize(buffer.size() * 2);
        }
    }

    static std::optional<std::filesystem::path> env_path(const wchar_t* name) {
        const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
        if (required == 0) {
            return std::nullopt;
        }

        std::wstring value(required, L'\0');
        const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
        if (copied == 0) {
            return std::nullopt;
        }

        value.resize(copied);
        return std::filesystem::path(value);
    }

    static std::optional<std::filesystem::path> service_exe_path() {
        std::error_code ec;
        if (auto configured = env_path(L"LLAVON_IME_SERVICE_PATH")) {
            if (std::filesystem::is_regular_file(*configured, ec)) {
                return configured;
            }
        }

        if (auto module_dir = dll_dir()) {
            auto candidate = *module_dir / "llavon-ime-service.exe";
            if (std::filesystem::is_regular_file(candidate, ec)) {
                return candidate;
            }
        }

        return std::nullopt;
    }

    static bool connect_pipe() {
        hPipe = CreateFileW(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        return hPipe != INVALID_HANDLE_VALUE;
    }

    static bool launch_backend() {
        const auto exe_path = service_exe_path();
        if (!exe_path) {
            return false;
        }

        std::wstring command_line = L"\"" + exe_path->wstring() + L"\"";
        std::wstring working_dir = exe_path->parent_path().wstring();

        STARTUPINFOW startup_info{};
        startup_info.cb = sizeof(startup_info);

        PROCESS_INFORMATION process_info{};
        const BOOL ok =
            CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           working_dir.empty() ? nullptr : working_dir.c_str(), &startup_info, &process_info);
        if (!ok) {
            return false;
        }

        CloseHandle(process_info.hThread);
        CloseHandle(process_info.hProcess);
        return true;
    }

    static bool retry_connect_pipe() {
        for (int i = 0; i < 20; ++i) {
            if (connect_pipe()) {
                return true;
            }
            Sleep(100);
        }
        return false;
    }

    static bool ensure_pipe() {
        if (hPipe != INVALID_HANDLE_VALUE) return true;
        if (connect_pipe()) return true;

        HANDLE launch_mutex = CreateMutexW(nullptr, FALSE, launch_mutex_name);
        if (!launch_mutex) {
            launch_backend();
            return retry_connect_pipe();
        }

        const DWORD wait_result = WaitForSingleObject(launch_mutex, 0);
        const bool owns_launch = wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED;
        if (owns_launch) {
            if (!connect_pipe()) {
                launch_backend();
            }
            const bool connected = retry_connect_pipe();
            ReleaseMutex(launch_mutex);
            CloseHandle(launch_mutex);
            return connected;
        }

        CloseHandle(launch_mutex);
        return retry_connect_pipe();
    }

    static void disconnect() {
        if (hPipe != INVALID_HANDLE_VALUE) {
            CloseHandle(hPipe);
            hPipe = INVALID_HANDLE_VALUE;
        }
    }

    template <typename T>
    static bool write_exact(const T& val) {
        DWORD written = 0;
        return WriteFile(hPipe, &val, sizeof(T), &written, nullptr) && written == sizeof(T);
    }

    static bool write_exact(const void* buf, DWORD size) {
        DWORD written = 0;
        while (written < size) {
            DWORD n = 0;
            if (!WriteFile(hPipe, static_cast<const char*>(buf) + written, size - written, &n, nullptr)) return false;
            written += n;
        }
        return true;
    }

    template <typename T>
    static bool read_exact(T& val) {
        DWORD total = 0;
        while (total < sizeof(T)) {
            DWORD n = 0;
            if (!ReadFile(hPipe, reinterpret_cast<char*>(&val) + total, sizeof(T) - total, &n, nullptr) || n == 0)
                return false;
            total += n;
        }
        return true;
    }

    static bool read_exact(void* buf, DWORD size) {
        DWORD total = 0;
        while (total < size) {
            DWORD n = 0;
            if (!ReadFile(hPipe, static_cast<char*>(buf) + total, size - total, &n, nullptr) || n == 0) return false;
            total += n;
        }
        return true;
    }

    static bool write_command(PipeCommand command) {
        const uint8_t raw_command = static_cast<uint8_t>(command);
        return write_exact(raw_command);
    }

    static bool read_input_mode(InputMode& mode) {
        uint8_t raw_mode = 0;
        if (!read_exact(raw_mode)) return false;

        mode = raw_mode == static_cast<uint8_t>(InputMode::English) ? InputMode::English : InputMode::Chinese;
        return true;
    }

public:
    void ready() override {
        if (!ensure_pipe()) return;

        if (!write_command(PipeCommand::Ready)) { disconnect(); return; }

        uint8_t ok = 0;
        if (!read_exact(ok)) { disconnect(); return; }
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
        if (!read_exact(resp_cnt)) { disconnect(); return; }

        for (uint32_t i = 0; i < resp_cnt && i < static_cast<uint32_t>(padding.size()); i++) {
            uint32_t cand_cnt;
            if (!read_exact(cand_cnt)) { disconnect(); return; }

            std::vector<char32_t> cands(cand_cnt);
            if (cand_cnt > 0 && !read_exact(cands.data(), cand_cnt * sizeof(char32_t))) { disconnect(); return; }

            if (!padding[i].chosen) {
                padding[i].set_candaiates(std::move(cands));
            }
            padding[i].predicted = true;
        }
    }

    InputMode toggle_input_mode() override {
        if (!ensure_pipe()) return InputMode::Chinese;

        if (!write_command(PipeCommand::ToggleInputMode)) { disconnect(); return InputMode::Chinese; }

        InputMode mode = InputMode::Chinese;
        if (!read_input_mode(mode)) { disconnect(); return InputMode::Chinese; }
        return mode;
    }

    InputMode current_input_mode() override {
        if (!ensure_pipe()) return InputMode::Chinese;

        if (!write_command(PipeCommand::GetInputMode)) { disconnect(); return InputMode::Chinese; }

        InputMode mode = InputMode::Chinese;
        if (!read_input_mode(mode)) { disconnect(); return InputMode::Chinese; }
        return mode;
    }
};

}  // namespace tsf
