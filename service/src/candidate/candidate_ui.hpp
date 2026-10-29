#pragma once

#include "../ui/ui_thread.hpp"

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llavon::candidate {

struct Presentation {
    HWND owner_window = nullptr;
    int anchor_x = 0;
    int anchor_y = 0;
    int anchor_top = 0;
    std::vector<std::wstring> candidates;
    std::uint32_t selection_index = 0;
    std::uint32_t layout_columns = 1;
    std::uint32_t number_column = 0;
    bool can_prev_page = false;
    bool can_next_page = false;
};

class CandidateUi final {
public:
    explicit CandidateUi(ui::UiThread& thread);
    ~CandidateUi();
    CandidateUi(const CandidateUi&) = delete;
    CandidateUi& operator=(const CandidateUi&) = delete;

    // Copies/moves an owned snapshot, then queues presentation on the UI STA.
    bool present(Presentation presentation);
    void hide() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace llavon::candidate
