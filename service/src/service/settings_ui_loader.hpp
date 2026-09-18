#pragma once

#include <ime-core/core.hpp>
#include <windows.h>

#include "user_settings.hpp"
#include "../settings/settings_ui_api.h"

#include <cstdint>
#include <functional>
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
    using SaveCustomNames =
        std::function<bool(const std::vector<CustomNameSetting>&)>;
    using SaveWidthToggleSetting = std::function<bool(bool)>;

    void configure(
        const std::vector<llavon::ime::core::InferenceDeviceInfo>& devices,
        llavon::ime::core::InferenceDeviceSelection selected,
        const llavon::ime::core::InferenceRuntimeInfo& active,
        SaveInferenceSettings save_settings,
        std::vector<CustomNameSetting> custom_names,
        SaveCustomNames save_custom_names,
        bool shift_space_width_toggle_enabled,
        SaveWidthToggleSetting save_width_toggle);
    bool show();
    bool show_context_menu(POINT location);

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

    using ConfigureFunction = std::int32_t (*)(
        const llavon_settings_inference_device*, std::size_t, std::int32_t,
        const char16_t*, const llavon_settings_inference_device*, std::int32_t,
        std::int32_t, llavon_settings_save_inference_callback, void*,
        const llavon_settings_custom_name*, std::size_t,
        llavon_settings_save_custom_names_callback, void*, std::int32_t,
        llavon_settings_save_width_toggle_callback, void*);
    using StartFunction = std::int32_t (*)();
    using ShowFunction = void (*)();
    using ShowContextMenuFunction = void (*)(std::int32_t, std::int32_t);
    using StopFunction = std::int32_t (*)();

    bool load();
    bool start();
    bool configure_module();
    static std::int32_t save_trampoline(
        void* context, std::int32_t backend, const char16_t* device_id) noexcept;
    static std::int32_t save_custom_names_trampoline(
        void* context, const llavon_settings_custom_name* custom_names,
        std::size_t custom_name_count) noexcept;
    static std::int32_t save_width_toggle_trampoline(
        void* context, std::int32_t enabled) noexcept;
    void report_error(const wchar_t* detail) const noexcept;

    HMODULE module_ = nullptr;
    ConfigureFunction configure_ = nullptr;
    StartFunction start_ = nullptr;
    ShowFunction show_ = nullptr;
    ShowContextMenuFunction show_context_menu_ = nullptr;
    StopFunction stop_ = nullptr;
    std::vector<DeviceStorage> devices_;
    DeviceStorage active_device_;
    bool gpu_offload_ = false;
    bool fell_back_to_cpu_ = false;
    llavon::ime::core::InferenceDeviceSelection selected_;
    SaveInferenceSettings save_settings_;
    std::vector<CustomNameStorage> custom_names_;
    SaveCustomNames save_custom_names_;
    bool shift_space_width_toggle_enabled_ = false;
    SaveWidthToggleSetting save_width_toggle_;
    bool started_ = false;
};

}  // namespace llavon::service
