#pragma once

#include "settings_ui_api.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llavon::settings {

struct InferenceDeviceOption {
    std::int32_t backend = LLAVON_SETTINGS_BACKEND_AUTO;
    std::int32_t device_type = LLAVON_SETTINGS_DEVICE_CPU;
    std::wstring device_id;
    std::wstring name;
    std::wstring description;
    std::uint64_t memory_total = 0;
};

struct SettingsConfiguration {
    std::vector<InferenceDeviceOption> devices;
    std::int32_t selected_backend = LLAVON_SETTINGS_BACKEND_AUTO;
    std::wstring selected_device_id;
    InferenceDeviceOption active_device;
    bool gpu_offload = false;
    bool fell_back_to_cpu = false;
    llavon_settings_save_inference_callback save_callback = nullptr;
    void* save_context = nullptr;
};

}  // namespace llavon::settings
