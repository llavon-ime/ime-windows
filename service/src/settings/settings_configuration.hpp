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

struct TrainingDataOption {
    std::u16string event_id;
    std::u16string context;
    std::u16string answer;
    std::u16string reading;
    bool revice = false;
};

struct LoraHistoryOption {
    std::u16string completed_at_utc;
    std::size_t record_count = 0;
    std::size_t cumulative_record_count = 0;
    std::int64_t optimizer_steps = 0;
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
    std::u16string model_path;
    llavon_settings_save_model_path_callback save_model_path_callback = nullptr;
    void* save_model_path_context = nullptr;
    std::vector<CustomNameOption> custom_names;
    llavon_settings_save_custom_names_callback save_custom_names_callback = nullptr;
    void* save_custom_names_context = nullptr;
    bool shift_space_width_toggle_enabled = false;
    llavon_settings_save_width_toggle_callback save_width_toggle_callback = nullptr;
    void* save_width_toggle_context = nullptr;
    std::vector<TrainingDataOption> training_items;
    llavon_settings_refresh_training_items_callback refresh_training_items_callback = nullptr;
    void* refresh_training_items_context = nullptr;
    llavon_settings_get_lora_history_callback get_lora_history_callback = nullptr;
    void* get_lora_history_context = nullptr;
    llavon_settings_protection_callback protection_callback = nullptr;
    void* protection_context = nullptr;
    llavon_settings_start_lora_training_callback start_lora_training_callback = nullptr;
    void* start_lora_training_context = nullptr;
    llavon_settings_get_lora_status_callback get_lora_status_callback = nullptr;
    void* get_lora_status_context = nullptr;
    llavon_settings_lora_model_action_callback lora_model_action_callback = nullptr;
    void* lora_model_action_context = nullptr;
    llavon_settings_cancel_lora_callback cancel_lora_callback = nullptr;
    void* cancel_lora_context = nullptr;
};

}  // namespace llavon::settings
