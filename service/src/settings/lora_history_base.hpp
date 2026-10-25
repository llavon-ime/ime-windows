#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llavon::settings {

struct LoraHistoryRunView {
    std::int64_t id = 0;
    std::int64_t parent_id = 0;
    std::string base_model_fingerprint;
    std::string base_model_revision;
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

struct LoraHistoryBaseView {
    std::int64_t id = -1;
    std::string fingerprint;
    std::string revision;
    std::u16string output_model_path;
    bool current = false;
};

inline std::size_t find_lora_base_selection(
    const std::vector<std::int64_t>& ids,
    const std::vector<std::string>& fingerprints,
    std::int64_t run_id,
    std::string_view root_fingerprint) {
    if (run_id > 0) {
        const auto found = std::ranges::find(ids, run_id);
        if (found != ids.end()) return static_cast<std::size_t>(found - ids.begin());
    } else if (!root_fingerprint.empty()) {
        const auto found = std::ranges::find(fingerprints, root_fingerprint);
        if (found != fingerprints.end())
            return static_cast<std::size_t>(found - fingerprints.begin());
    }
    return 0;
}

inline std::vector<LoraHistoryBaseView> lora_history_bases(
    const std::vector<LoraHistoryRunView>& runs,
    std::optional<LoraHistoryBaseView> current_base = std::nullopt,
    const std::vector<LoraHistoryBaseView>& catalog = {}) {
    std::vector<LoraHistoryBaseView> bases;
    if (current_base && !current_base->fingerprint.empty()) {
        current_base->id = -2;
        current_base->current = true;
        bases.push_back(*current_base);
    }
    for (const auto& known : catalog) {
        if (known.fingerprint.empty() || std::ranges::any_of(bases, [&](const auto& base) {
                return base.fingerprint == known.fingerprint;
            })) continue;
        auto base = known;
        base.id = -2 - static_cast<std::int64_t>(bases.size());
        base.current = current_base && base.fingerprint == current_base->fingerprint;
        bases.push_back(std::move(base));
    }
    for (const auto& run : runs) {
        if (std::ranges::any_of(bases, [&](const auto& base) {
                return base.fingerprint == run.base_model_fingerprint;
            })) continue;
        bases.push_back(LoraHistoryBaseView{
            .id = -2 - static_cast<std::int64_t>(bases.size()),
            .fingerprint = run.base_model_fingerprint,
            .revision = run.base_model_revision,
        });
    }
    return bases;
}

}  // namespace llavon::settings
