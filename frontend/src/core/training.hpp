#pragma once

#include <optional>
#include <string>

#include "core/buffer.hpp"
#include "engine/engine_types.hpp"

namespace tsf {

// Only text that went into the document exactly as the model wrote it, or as
// the user picked it, teaches the model.
inline std::optional<CommitSample> training_sample(const CompositionBuffer& buffer, std::u16string preceding) {
    if (!buffer.output_is_trusted()) return std::nullopt;
    return buffer.commit_sample(std::move(preceding));
}

}  // namespace tsf
