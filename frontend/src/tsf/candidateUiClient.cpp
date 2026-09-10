#include "candidateUiClient.hpp"

#include "system/globals.h"

#include <filesystem>
#include <optional>
#include <string>

namespace tsf {
namespace {

std::optional<std::filesystem::path> module_directory() {
    if (!Globals::hinstance) {
        return std::nullopt;
    }

    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(
            Globals::hinstance, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return std::nullopt;
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
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

std::optional<std::filesystem::path> service_executable_path() {
    std::error_code error;
    if (auto configured = environment_path(L"LLAVON_IME_SERVICE_PATH")) {
        if (std::filesystem::is_regular_file(*configured, error)) {
            return configured;
        }
    }
    if (auto directory = module_directory()) {
        auto candidate = *directory / "llavon-ime-service.exe";
        if (std::filesystem::is_regular_file(candidate, error)) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace

CandidateUiClient::~CandidateUiClient() {
    hide();
    disconnect();
}

bool CandidateUiClient::present(const CandidateUiPresentation& presentation) {
    const uint32_t candidate_count = static_cast<uint32_t>(presentation.candidates.size());
    if (candidate_count == 0 || candidate_count > maximum_candidate_count ||
        presentation.layout_columns == 0 ||
        presentation.layout_columns > maximum_layout_columns ||
        presentation.number_column >= presentation.layout_columns ||
        presentation.selection_index >= candidate_count ||
        candidate_count > presentation.layout_columns * page_size) {
        return false;
    }
    for (const auto& candidate : presentation.candidates) {
        if (candidate.size() > maximum_candidate_length) {
            return false;
        }
    }
    if (!ensure_pipe()) {
        return false;
    }

    const uint8_t can_prev_page = presentation.can_prev_page ? 1 : 0;
    const uint8_t can_next_page = presentation.can_next_page ? 1 : 0;
    if (!write_command(Command::Present) || !write_value(presentation.owner_window) ||
        !write_value(presentation.anchor_x) ||
        !write_value(presentation.anchor_y) || !write_value(presentation.anchor_top) ||
        !write_value(candidate_count) ||
        !write_value(presentation.selection_index) ||
        !write_value(presentation.layout_columns) ||
        !write_value(presentation.number_column) || !write_value(can_prev_page) ||
        !write_value(can_next_page)) {
        disconnect();
        return false;
    }

    for (const auto& candidate : presentation.candidates) {
        const uint32_t length = static_cast<uint32_t>(candidate.size());
        if (!write_value(length) ||
            (length != 0 &&
             !write_bytes(candidate.data(), length * static_cast<DWORD>(sizeof(wchar_t))))) {
            disconnect();
            return false;
        }
    }
    return true;
}

void CandidateUiClient::hide() noexcept {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return;
    }
    if (!write_command(Command::Hide)) {
        disconnect();
    }
}

void CandidateUiClient::disconnect() noexcept {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return;
    }
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
}

bool CandidateUiClient::ensure_pipe() {
    if (pipe_ != INVALID_HANDLE_VALUE || connect_pipe()) {
        return true;
    }

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

bool CandidateUiClient::connect_pipe() {
    pipe_ = CreateFileW(pipe_name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (pipe_ == INVALID_HANDLE_VALUE) {
        return false;
    }
    return true;
}

bool CandidateUiClient::retry_connect_pipe() {
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (connect_pipe()) {
            return true;
        }
        Sleep(100);
    }
    return false;
}

bool CandidateUiClient::launch_backend() const {
    const auto executable = service_executable_path();
    if (!executable) {
        return false;
    }

    std::wstring command_line = L"\"" + executable->wstring() + L"\"";
    const std::wstring working_directory = executable->parent_path().wstring();
    STARTUPINFOW startup_info{};
    startup_info.cb = sizeof(startup_info);
    PROCESS_INFORMATION process_info{};
    const BOOL created = CreateProcessW(
        nullptr, command_line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
        working_directory.empty() ? nullptr : working_directory.c_str(), &startup_info,
        &process_info);
    if (!created) {
        return false;
    }

    CloseHandle(process_info.hThread);
    CloseHandle(process_info.hProcess);
    return true;
}

bool CandidateUiClient::write_command(Command command) {
    const uint8_t raw_command = static_cast<uint8_t>(command);
    return write_value(raw_command) && write_value(protocol_version);
}

bool CandidateUiClient::write_bytes(const void* data, DWORD size) {
    DWORD total = 0;
    while (total < size) {
        DWORD written = 0;
        if (!WriteFile(
                pipe_, static_cast<const char*>(data) + total, size - total, &written,
                nullptr) || written == 0) {
            return false;
        }
        total += written;
    }
    return true;
}

}  // namespace tsf
