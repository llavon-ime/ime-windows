#pragma once

#include <ime-core/core.hpp>

namespace llavon::service {

llavon::ime::core::InferenceDeviceSelection load_inference_settings() noexcept;
bool save_inference_settings(
    const llavon::ime::core::InferenceDeviceSelection& selection) noexcept;

}  // namespace llavon::service
