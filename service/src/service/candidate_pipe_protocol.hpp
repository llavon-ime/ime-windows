#pragma once

#include <cstdint>

namespace llavon::service::candidate_pipe_protocol {

inline constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime-candidate-ui";
inline constexpr uint16_t protocol_version = 3;
inline constexpr uint32_t maximum_candidate_count = 36;
inline constexpr uint32_t maximum_candidate_length = 256;
inline constexpr uint32_t maximum_layout_columns = 4;
inline constexpr uint32_t page_size = 9;

enum class Command : uint8_t {
    Present = 1,
    Hide = 2,
};

constexpr bool valid_presentation_header(
    uint32_t candidate_count,
    uint32_t selection_index,
    uint32_t layout_columns,
    uint32_t number_column,
    uint8_t can_prev_page,
    uint8_t can_next_page) noexcept {
    return candidate_count != 0 && candidate_count <= maximum_candidate_count &&
           layout_columns != 0 && layout_columns <= maximum_layout_columns &&
           number_column < layout_columns && selection_index < candidate_count &&
           candidate_count <= layout_columns * page_size && can_prev_page <= 1 &&
           can_next_page <= 1;
}

}  // namespace llavon::service::candidate_pipe_protocol
