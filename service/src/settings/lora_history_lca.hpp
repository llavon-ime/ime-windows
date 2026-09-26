#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace llavon::settings {

struct LoraHistoryParent {
    std::int64_t id;
    std::int64_t parent_id;
};

// Base model has ID 0. Returns nullopt for unknown IDs or invalid ancestry.
std::optional<std::int64_t> tarjan_lca(
    std::span<const LoraHistoryParent> runs,
    std::int64_t first_id,
    std::int64_t second_id);

}  // namespace llavon::settings
