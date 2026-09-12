#pragma once

#include <ime-core/core.hpp>

#include <string>
#include <vector>

namespace llavon::service {

struct CustomNameSetting {
    std::u32string name;
    std::vector<std::u16string> readings;

    bool operator==(const CustomNameSetting&) const = default;
};

struct UserSettings {
    llavon::ime::core::InferenceDeviceSelection inference;
    std::vector<CustomNameSetting> custom_names;
};

UserSettings load_settings() noexcept;

bool save_inference_settings(
    const llavon::ime::core::InferenceDeviceSelection& selection) noexcept;
bool save_custom_names(
    const std::vector<CustomNameSetting>& custom_names) noexcept;

}  // namespace llavon::service
