#pragma once

#include <algorithm>
#include <cstdint>
#include <expected>

namespace llavon::service {

struct AmdClockRange {
    std::int32_t minValue;
    std::int32_t maxValue;
    std::int32_t step;
};

enum class AmdClockTargetError { invalid_values, offset_or_unknown_maximum };

// ADLX uses absolute MHz for the minimum, but Navi4+ exposes the maximum as
// an offset. Only accept an unambiguously positive absolute-frequency range.
// A range containing zero/negative values is not a usable absolute ceiling;
// do not invent a base clock or substitute the overclocking range's maximum.
constexpr std::expected<std::int32_t, AmdClockTargetError> amd_gpu_clock_target(
    AmdClockRange minimum, AmdClockRange maximum,
    std::int32_t current_min, std::int32_t current_max) noexcept {
    if (minimum.minValue < 0 || minimum.maxValue < minimum.minValue ||
        minimum.step <= 0 || maximum.maxValue < maximum.minValue ||
        maximum.step <= 0 || current_min < minimum.minValue ||
        current_min > minimum.maxValue || current_max < maximum.minValue ||
        current_max > maximum.maxValue) {
        return std::unexpected(AmdClockTargetError::invalid_values);
    }
    if (maximum.minValue <= 0) {
        return std::unexpected(AmdClockTargetError::offset_or_unknown_maximum);
    }
    const auto ceiling = std::min(minimum.maxValue, current_max);
    if (current_min >= ceiling) return current_min;

    const auto desired = static_cast<std::int64_t>(current_min) +
                         (static_cast<std::int64_t>(ceiling) - current_min) * 3 / 4;
    const auto steps = (desired - minimum.minValue) / minimum.step;
    const auto target = static_cast<std::int32_t>(minimum.minValue + steps * minimum.step);
    return std::max(current_min, target);
}

} // namespace llavon::service
