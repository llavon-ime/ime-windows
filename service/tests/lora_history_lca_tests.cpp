#include "settings/lora_history_lca.hpp"
#include "settings/lora_history_base.hpp"

#include <cstdint>
#include <vector>

using llavon::settings::LoraHistoryParent;
using llavon::settings::tarjan_lca;

int main() {
    const auto fingerprint_a = std::string(64, 'a');
    const auto fingerprint_b = std::string(64, 'b');
    const llavon::settings::LoraHistoryBaseView current_b{
        .fingerprint = fingerprint_b, .revision = "revision-b"};
    const std::vector<llavon::settings::LoraHistoryRunView> only_a{
        {.id = 1, .base_model_fingerprint = fingerprint_a,
         .base_model_revision = "revision-a"}};
    const auto updated = llavon::settings::lora_history_bases(only_a, current_b);
    if (updated.size() != 2 || !updated[0].current ||
        updated[0].fingerprint != fingerprint_b ||
        updated[1].fingerprint != fingerprint_a) return 15;
    auto a_and_b = only_a;
    a_and_b.push_back({.id = 2, .base_model_fingerprint = fingerprint_b,
                       .base_model_revision = "revision-b"});
    const auto deduplicated = llavon::settings::lora_history_bases(a_and_b, current_b);
    if (deduplicated.size() != 2 || deduplicated[0].fingerprint != fingerprint_b)
        return 16;
    const auto empty_current = llavon::settings::lora_history_bases({}, current_b);
    if (empty_current.size() != 1 || !empty_current[0].current ||
        empty_current[0].id >= 0) return 17;
    const std::vector<llavon::settings::LoraHistoryBaseView> catalog{
        {.fingerprint = fingerprint_a, .revision = "revision-a"},
        {.fingerprint = fingerprint_b, .revision = "stale-b"},
    };
    const auto catalog_roots = llavon::settings::lora_history_bases(
        {}, current_b, catalog);
    if (catalog_roots.size() != 2 || !catalog_roots[0].current ||
        catalog_roots[0].fingerprint != fingerprint_b ||
        catalog_roots[0].revision != "revision-b" ||
        catalog_roots[1].fingerprint != fingerprint_a) return 21;
    const std::vector<std::int64_t> refreshed_ids{-2, -3, 9};
    const std::vector<std::string> refreshed_fingerprints{
        fingerprint_b, fingerprint_a, ""};
    if (llavon::settings::find_lora_base_selection(refreshed_ids,
            refreshed_fingerprints, -8, fingerprint_a) != 1) return 18;
    if (llavon::settings::find_lora_base_selection(refreshed_ids,
            refreshed_fingerprints, 9, {}) != 2) return 19;
    if (llavon::settings::find_lora_base_selection(refreshed_ids,
            refreshed_fingerprints, -8, std::string(64, 'c')) != 0) return 20;
    // Latest and previous runs are siblings across two branches.
    const std::vector<LoraHistoryParent> runs{
        {1, 0}, {2, 1}, {3, 1}, {4, 2}, {5, 3},
    };
    if (tarjan_lca(runs, 5, 4) != 1) return 1;
    if (tarjan_lca(runs, 4, 5) != 1) return 2;
    if (tarjan_lca(runs, 4, 2) != 2) return 3;
    if (tarjan_lca(runs, 5, 5) != 5) return 4;
    if (tarjan_lca(runs, 5, 0) != 0) return 5;
    if (tarjan_lca(runs, 5, 99)) return 6;

    const std::vector<LoraHistoryParent> root_branches{{1, 0}, {2, 0}};
    if (tarjan_lca(root_branches, 1, 2) != 0) return 7;

    const std::vector<LoraHistoryParent> missing_parent{{1, 99}};
    if (tarjan_lca(missing_parent, 1, 0)) return 8;
    const std::vector<LoraHistoryParent> cycle{{1, 2}, {2, 1}};
    if (tarjan_lca(cycle, 1, 2)) return 9;
    const std::vector<LoraHistoryParent> duplicate{{1, 0}, {1, 0}};
    if (tarjan_lca(duplicate, 1, 0)) return 10;
    const std::vector<LoraHistoryParent> separate_models{
        {1, -2}, {2, -2}, {3, -3}};
    if (tarjan_lca(separate_models, 1, 2) != -2) return 12;
    if (tarjan_lca(separate_models, 1, 3)) return 13;
    if (tarjan_lca(separate_models, -2, 1) != -2) return 14;

    std::vector<LoraHistoryParent> long_chain;
    long_chain.reserve(10'000);
    for (std::int64_t id = 1; id <= 10'000; ++id) {
        long_chain.push_back({id, id - 1});
    }
    if (tarjan_lca(long_chain, 10'000, 9'999) != 9'999) return 11;
    return 0;
}
