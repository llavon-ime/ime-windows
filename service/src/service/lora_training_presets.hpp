#pragma once

#include <cstdint>

namespace llavon::service {

// Version 1 of the provisional product presets. Keep adapter structure fixed
// across strengths so a user can explicitly continue a compatible adapter.
enum class LoraTrainingStrength : std::int32_t {
    ultra_low = 0,
    low = 1,
    medium = 2,
    high = 3,
    advanced = 4,
};

struct LoraTrainingPreset {
    double learning_rate;
    std::int32_t epochs;
};

constexpr LoraTrainingPreset lora_training_preset(
    LoraTrainingStrength strength) noexcept {
    switch (strength) {
        case LoraTrainingStrength::ultra_low: return {1e-8, 1};
        case LoraTrainingStrength::medium: return {3e-5, 2};
        case LoraTrainingStrength::high: return {1e-4, 5};
        case LoraTrainingStrength::low:
        case LoraTrainingStrength::advanced: return {1e-5, 1};
    }
    return {1e-5, 1};
}

}  // namespace llavon::service
