#pragma once

#include <ime-core/core.hpp>
#include <windows.h>

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

    void configure(
        const std::vector<llavon::ime::core::InferenceDeviceInfo>& devices,
        llavon::ime::core::InferenceDeviceSelection selected,
        const llavon::ime::core::InferenceRuntimeInfo& active,
        SaveInferenceSettings save_settings);
    bool show();

private:
    struct DeviceStorage {
        std::int32_t backend = 0;
        std::int32_t device_type = 0;
        std::wstring device_id;
        std::wstring name;
        std::wstring description;
        std::uint64_t memory_total = 0;
    };

    using ConfigureFunction = std::int32_t (*)(
        const llavon_settings_inference_device*, std::size_t, std::int32_t,
        const wchar_t*, const llavon_settings_inference_device*, std::int32_t,
        std::int32_t, llavon_settings_save_inference_callback, void*);
    using StartFunction = std::int32_t (*)();
    using ShowFunction = void (*)();
    using StopFunction = std::int32_t (*)();

    bool load();
    bool configure_module();
    static std::int32_t save_trampoline(
        void* context, std::int32_t backend, const wchar_t* device_id) noexcept;
    void report_error(const wchar_t* detail) const noexcept;

    HMODULE module_ = nullptr;
    ConfigureFunction configure_ = nullptr;
    StartFunction start_ = nullptr;
    ShowFunction show_ = nullptr;
    StopFunction stop_ = nullptr;
    std::vector<DeviceStorage> devices_;
    DeviceStorage active_device_;
    bool gpu_offload_ = false;
    bool fell_back_to_cpu_ = false;
    llavon::ime::core::InferenceDeviceSelection selected_;
    SaveInferenceSettings save_settings_;
    bool started_ = false;
};

}  // namespace llavon::service
