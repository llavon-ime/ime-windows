#include "settings/lora_history_lca.hpp"

#include <cstdint>
#include <vector>

using llavon::settings::LoraHistoryParent;
using llavon::settings::tarjan_lca;

int main() {
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

    std::vector<LoraHistoryParent> long_chain;
    long_chain.reserve(10'000);
    for (std::int64_t id = 1; id <= 10'000; ++id) {
        long_chain.push_back({id, id - 1});
    }
    if (tarjan_lca(long_chain, 10'000, 9'999) != 9'999) return 11;
    return 0;
}
