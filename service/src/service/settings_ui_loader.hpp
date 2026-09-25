#pragma once

#include <ime-core/core.hpp>
#include <windows.h>

#include "user_settings.hpp"
#include "training_data_writer.hpp"
#include "lora_training_manager.hpp"
#include "../settings/settings_ui_api.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace llavon::service {

class SettingsUiLoader final {
public:
    SettingsUiLoader() = default;
    SettingsUiLoader(const SettingsUiLoader&) = delete;
    SettingsUiLoader& operator=(const SettingsUiLoader&) = delete;
    ~SettingsUiLoader();

    using SaveInferenceSettings = std::function<bool(
        const llavon::ime::core::InferenceDeviceSelection&)>;
    using SaveModelPath = std::function<bool(const std::filesystem::path&)>;
    using SaveCustomNames =
        std::function<bool(const std::vector<CustomNameSetting>&)>;
    using SaveWidthToggleSetting = std::function<bool(bool)>;
    using LoadTrainingData = std::function<std::vector<TrainingDataItem>(std::string_view)>;
    using DeleteTrainingData = std::function<bool(std::u16string_view)>;
    using LoadLoraHistory = std::function<std::vector<LoraTrainingRun>()>;
    using StartLoraTraining = std::function<bool(
        const std::vector<std::u16string>&,
        const std::vector<std::u16string>&,
        const llavon_settings_lora_options&, std::string_view)>;
    using ProtectionAction = std::function<std::size_t(int, std::string_view)>;
    using GetLoraStatus = std::function<LoraOperationStatus()>;
    using LoraModelAction = std::function<bool(std::int32_t)>;
    using CancelLora = std::function<void()>;

    void configure(
        const std::vector<llavon::ime::core::InferenceDeviceInfo>& devices,
        llavon::ime::core::InferenceDeviceSelection selected,
        const llavon::ime::core::InferenceRuntimeInfo& active,
        SaveInferenceSettings save_settings,
        std::u16string model_path,
        SaveModelPath save_model_path,
        std::vector<CustomNameSetting> custom_names,
        SaveCustomNames save_custom_names,
        bool shift_space_width_toggle_enabled,
        SaveWidthToggleSetting save_width_toggle,
        LoadTrainingData load_training_data,
        DeleteTrainingData delete_training_data,
        LoadLoraHistory load_lora_history,
        StartLoraTraining start_lora_training,
        GetLoraStatus get_lora_status,
        LoraModelAction lora_model_action,
        CancelLora cancel_lora, ProtectionAction protection_action);
    bool show();
    bool show_context_menu(POINT location);
    void notify_pending_count(std::size_t count) noexcept;

private:
    struct DeviceStorage {
        std::int32_t backend = 0;
        std::int32_t device_type = 0;
        std::u16string device_id;
        std::u16string name;
        std::u16string description;
        std::uint64_t memory_total = 0;
    };

    struct CustomNameStorage {
        std::u16string name;
        std::vector<std::u16string> readings;
    };

    struct TrainingDataStorage {
        std::u16string event_id;
        std::u16string context;
        std::u16string answer;
        std::u16string reading;
        bool revice = false;
    };

    struct LoraHistoryStorage {
        std::u16string completed_at_utc;
        std::size_t record_count = 0;
        std::size_t cumulative_record_count = 0;
        std::int64_t optimizer_steps = 0;
    };

    using ConfigureFunction = std::int32_t (*)(
        const llavon_settings_inference_device*, std::size_t, std::int32_t,
        const char16_t*, const llavon_settings_inference_device*, std::int32_t,
        std::int32_t, llavon_settings_save_inference_callback, void*,
        const char16_t*, llavon_settings_save_model_path_callback, void*,
        const llavon_settings_custom_name*, std::size_t,
        llavon_settings_save_custom_names_callback, void*, std::int32_t,
        llavon_settings_save_width_toggle_callback, void*,
        const llavon_settings_training_item*, std::size_t,
        llavon_settings_refresh_training_items_callback, void*,
        llavon_settings_delete_training_item_callback, void*,
        llavon_settings_get_lora_history_callback, void*,
        llavon_settings_start_lora_training_callback, void*,
        llavon_settings_get_lora_status_callback, void*,
        llavon_settings_lora_model_action_callback, void*,
        llavon_settings_cancel_lora_callback, void*, llavon_settings_protection_callback, void*);
    using StartFunction = std::int32_t (*)();
    using ShowFunction = void (*)();
    using ShowContextMenuFunction = void (*)(std::int32_t, std::int32_t);
    using StopFunction = std::int32_t (*)();
    using SetPendingCountFunction = void (*)(std::size_t);

    bool load();
    bool start();
    bool configure_module();
    void refresh_training_items(std::string_view password = {});
    static std::int32_t save_trampoline(
        void* context, std::int32_t backend, const char16_t* device_id) noexcept;
    static std::int32_t save_custom_names_trampoline(
        void* context, const llavon_settings_custom_name* custom_names,
        std::size_t custom_name_count) noexcept;
    static std::int32_t save_model_path_trampoline(
        void* context, const char16_t* model_path) noexcept;
    static std::int32_t save_width_toggle_trampoline(
        void* context, std::int32_t enabled) noexcept;
    static std::int32_t start_lora_training_trampoline(
        void* context, const char16_t* const* selected_event_ids,
        std::size_t selected_event_id_count,
        const llavon_settings_lora_options* options, const char16_t* password) noexcept;
    static std::int32_t refresh_training_items_trampoline(
        void* context, llavon_settings_training_item* items,
        std::size_t item_capacity, std::size_t* item_count, const char16_t* password) noexcept;
    static std::int32_t delete_training_item_trampoline(
        void* context, const char16_t* event_id) noexcept;
    static std::int32_t protection_trampoline(void*, std::int32_t, const char16_t*, std::size_t*) noexcept;
    static std::int32_t get_lora_history_trampoline(
        void* context, llavon_settings_lora_history_item* items,
        std::size_t item_capacity, std::size_t* item_count) noexcept;
    static std::int32_t get_lora_status_trampoline(
        void* context, llavon_settings_lora_status* status) noexcept;
    static std::int32_t lora_model_action_trampoline(
        void* context, std::int32_t download_or_update) noexcept;
    static void cancel_lora_trampoline(void* context) noexcept;
    void report_error(const wchar_t* detail) const noexcept;

    HMODULE module_ = nullptr;
    ConfigureFunction configure_ = nullptr;
    StartFunction start_ = nullptr;
    ShowFunction show_ = nullptr;
    ShowContextMenuFunction show_context_menu_ = nullptr;
    StopFunction stop_ = nullptr;
    std::mutex pending_count_mutex_;
    std::optional<std::size_t> latest_pending_count_;
    SetPendingCountFunction set_pending_count_ = nullptr;
    std::vector<DeviceStorage> devices_;
    DeviceStorage active_device_;
    bool gpu_offload_ = false;
    bool fell_back_to_cpu_ = false;
    llavon::ime::core::InferenceDeviceSelection selected_;
    SaveInferenceSettings save_settings_;
    std::u16string model_path_;
    SaveModelPath save_model_path_;
    std::vector<CustomNameStorage> custom_names_;
    SaveCustomNames save_custom_names_;
    bool shift_space_width_toggle_enabled_ = false;
    SaveWidthToggleSetting save_width_toggle_;
    LoadTrainingData load_training_data_;
    DeleteTrainingData delete_training_data_;
    LoadLoraHistory load_lora_history_;
    StartLoraTraining start_lora_training_;
    GetLoraStatus get_lora_status_;
    LoraModelAction lora_model_action_;
    CancelLora cancel_lora_;
    ProtectionAction protection_action_;
    std::u16string lora_status_message_;
    std::u16string lora_status_revision_;
    std::u16string lora_status_output_path_;
    std::u16string lora_trainer_version_;
    std::u16string lora_trainer_backend_;
    std::u16string lora_trainer_release_version_;
    std::u16string lora_trainer_message_;
    std::vector<TrainingDataStorage> training_items_;
    std::vector<std::u16string> reviewed_event_ids_;
    std::vector<LoraHistoryStorage> lora_history_;
    bool started_ = false;
};

}  // namespace llavon::service
