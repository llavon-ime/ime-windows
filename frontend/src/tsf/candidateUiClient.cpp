#include "candidateUiClient.hpp"

#include "system/serviceLauncher.hpp"

#include <string>

namespace tsf {

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

    HANDLE launch_mutex = CreateMutexW(nullptr, FALSE, service_launch_mutex_name);
    if (!launch_mutex) {
        launch_service_backend();
        return retry_connect_pipe();
    }

    const DWORD wait_result = WaitForSingleObject(launch_mutex, 0);
    const bool owns_launch = wait_result == WAIT_OBJECT_0 || wait_result == WAIT_ABANDONED;
    if (owns_launch) {
        if (!connect_pipe()) {
            launch_service_backend();
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
