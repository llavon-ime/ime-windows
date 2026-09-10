#pragma once

#include <ime-core/core.hpp>

#include <optional>
#include <string>
#include <string_view>

namespace llavon::service::inference_settings_codec {

std::optional<llavon::ime::core::InferenceDeviceSelection> decode(
    std::string_view json);
std::string encode(const llavon::ime::core::InferenceDeviceSelection& selection);

}  // namespace llavon::service::inference_settings_codec
