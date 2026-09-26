#pragma once

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace llavon::settings {

struct LoraHistoryRunView {
    std::int64_t id = 0;
    std::int64_t parent_id = 0;
    std::wstring completed_local;
    std::u16string output_model_path;
    std::size_t record_count = 0;
    std::size_t cumulative_record_count = 0;
    std::int64_t optimizer_steps = 0;
    std::int32_t rank = 0;
    double alpha = 0;
    double dropout = 0;
    std::u16string target_modules;
    std::string training_request_json;
};

enum class LoraHistoryTreeAction {
    apply_model,
    choose_training_base,
};

struct LoraHistoryTreeView {
    winrt::Microsoft::UI::Xaml::Controls::Grid root{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button confirm{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ScrollViewer graph_view{nullptr};
};

// The returned XAML tree owns its node callbacks. Selecting a node only changes
// the pending choice; on_action runs when the user presses the footer button.
LoraHistoryTreeView make_lora_history_tree(
    std::vector<LoraHistoryRunView> runs,
    std::int64_t selected_id,
    std::int64_t applied_id,
    LoraHistoryTreeAction action,
    std::function<void(std::int64_t)> on_action);

} // namespace llavon::settings
