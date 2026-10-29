#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace llavon::service {

struct CandidateUiSnapshot final {
    std::uint64_t owner_window = 0;
    std::int32_t anchor_x = 0;
    std::int32_t anchor_y = 0;
    std::int32_t anchor_top = 0;
    std::vector<std::wstring> candidates;
    std::uint32_t selection_index = 0;
    std::uint32_t layout_columns = 1;
    std::uint32_t number_column = 0;
    bool can_prev_page = false;
    bool can_next_page = false;
};

} // namespace llavon::service
