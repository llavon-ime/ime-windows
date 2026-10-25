#pragma once

#include <cstdint>
#include <optional>
#include <span>

namespace llavon::settings {

struct LoraHistoryParent {
    std::int64_t id;
    std::int64_t parent_id;
};

// Content-identified base roots use negative IDs; 0 is the virtual forest
// root. Returns nullopt for nodes in different model trees, unknown IDs, or
// invalid ancestry. Legacy single-tree callers may still use base ID 0.
std::optional<std::int64_t> tarjan_lca(
    std::span<const LoraHistoryParent> runs,
    std::int64_t first_id,
    std::int64_t second_id);

}  // namespace llavon::settings
