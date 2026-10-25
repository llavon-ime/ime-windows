#pragma once

#include "lora_history_base.hpp"

#include <winrt/Microsoft.UI.Xaml.Controls.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace llavon::settings {

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
    std::optional<LoraHistoryBaseView> current_base,
    std::vector<LoraHistoryBaseView> catalog,
    std::function<void(std::int64_t)> on_action);

} // namespace llavon::settings
