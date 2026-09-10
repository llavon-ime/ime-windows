#pragma once

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "candidate/candidate_ui_api.h"

namespace llavon::service {

struct CandidateUiSnapshot final {
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

class CandidateUiLoader final {
public:
    CandidateUiLoader() = default;
    CandidateUiLoader(const CandidateUiLoader&) = delete;
    CandidateUiLoader& operator=(const CandidateUiLoader&) = delete;
    ~CandidateUiLoader();

    bool present(const CandidateUiSnapshot& snapshot);
    void hide() noexcept;

private:
    using StartFunction = std::int32_t (*)();
    using PresentFunction = std::int32_t (*)(const llavon_candidate_ui_presentation*);
    using HideFunction = void (*)();
    using StopFunction = std::int32_t (*)();

    bool load();
    bool start();
    void report_error(const wchar_t* detail, std::int32_t error) const noexcept;

    std::mutex mutex_;
    HMODULE module_ = nullptr;
    StartFunction start_ = nullptr;
    PresentFunction present_ = nullptr;
    HideFunction hide_ = nullptr;
    StopFunction stop_ = nullptr;
    bool started_ = false;
};

}  // namespace llavon::service
