#pragma once

#include "settings_ui_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llavon::settings {

struct InferenceDeviceOption {
    std::int32_t backend = LLAVON_SETTINGS_BACKEND_AUTO;
    std::int32_t device_type = LLAVON_SETTINGS_DEVICE_CPU;
    std::u16string device_id;
    std::u16string name;
    std::u16string description;
    std::uint64_t memory_total = 0;
};

struct CustomNameOption {
    std::u16string name;
    std::vector<std::u16string> readings;
};

struct SettingsConfiguration {
    std::vector<InferenceDeviceOption> devices;
    std::int32_t selected_backend = LLAVON_SETTINGS_BACKEND_AUTO;
    std::u16string selected_device_id;
    InferenceDeviceOption active_device;
    bool gpu_offload = false;
    bool fell_back_to_cpu = false;
    llavon_settings_save_inference_callback save_callback = nullptr;
    void* save_context = nullptr;
    std::vector<CustomNameOption> custom_names;
    llavon_settings_save_custom_names_callback save_custom_names_callback = nullptr;
    void* save_custom_names_context = nullptr;
};

}  // namespace llavon::settings
