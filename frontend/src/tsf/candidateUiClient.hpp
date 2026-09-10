#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace tsf {

struct CandidateUiPresentation final {
    uint64_t owner_window = 0;
    int32_t anchor_x = 0;
    int32_t anchor_y = 0;
    int32_t anchor_top = 0;
    std::vector<std::wstring> candidates;
    uint32_t selection_index = 0;
    uint32_t layout_columns = 1;
    uint32_t number_column = 0;
    bool can_prev_page = false;
    bool can_next_page = false;
};

class CandidateUiClient final {
public:
    CandidateUiClient() = default;
    CandidateUiClient(const CandidateUiClient&) = delete;
    CandidateUiClient& operator=(const CandidateUiClient&) = delete;
    ~CandidateUiClient();

    bool present(const CandidateUiPresentation& presentation);
    void hide() noexcept;
    void disconnect() noexcept;

private:
    enum class Command : uint8_t {
        Present = 1,
        Hide = 2,
    };

    bool ensure_pipe();
    bool connect_pipe();
    bool retry_connect_pipe();
    bool launch_backend() const;
    bool write_command(Command command);
    bool write_bytes(const void* data, DWORD size);

    template <typename Value>
    bool write_value(const Value& value) {
        return write_bytes(&value, static_cast<DWORD>(sizeof(value)));
    }

    static constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime-candidate-ui";
    static constexpr const wchar_t* launch_mutex_name = L"Local\\LlavonImeBackendStart";
    static constexpr uint16_t protocol_version = 3;
    static constexpr uint32_t maximum_candidate_count = 36;
    static constexpr uint32_t maximum_candidate_length = 256;
    static constexpr uint32_t maximum_layout_columns = 4;
    static constexpr uint32_t page_size = 9;

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

}  // namespace tsf
