#include "settings_ui_loader.hpp"

#include "../settings/settings_ui_api.h"

#include <utf8/cpp20.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llavon::service {
namespace {

constexpr wchar_t settings_ui_filename[] = L"llavon-ime-settings-ui.dll";

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) {
            return {};
        }
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

template <typename Function>
Function resolve(HMODULE module, const char* name) {
    return reinterpret_cast<Function>(GetProcAddress(module, name));
}

std::u16string utf8_to_utf16(std::string_view value) {
    return utf8::utf8to16(value);
}

std::int32_t backend_value(llavon::ime::core::InferenceBackend backend) {
    using llavon::ime::core::InferenceBackend;
    switch (backend) {
        case InferenceBackend::automatic:
            return LLAVON_SETTINGS_BACKEND_AUTO;
        case InferenceBackend::cpu:
            return LLAVON_SETTINGS_BACKEND_CPU;
        case InferenceBackend::cuda:
            return LLAVON_SETTINGS_BACKEND_CUDA;
        case InferenceBackend::vulkan:
            return LLAVON_SETTINGS_BACKEND_VULKAN;
        default:
            return LLAVON_SETTINGS_BACKEND_AUTO;
    }
}

std::int32_t device_type_value(llavon::ime::core::InferenceDeviceType type) {
    using llavon::ime::core::InferenceDeviceType;
    switch (type) {
        case InferenceDeviceType::gpu:
            return LLAVON_SETTINGS_DEVICE_GPU;
        case InferenceDeviceType::integrated_gpu:
            return LLAVON_SETTINGS_DEVICE_INTEGRATED_GPU;
        case InferenceDeviceType::cpu:
        default:
            return LLAVON_SETTINGS_DEVICE_CPU;
    }
}

llavon::ime::core::InferenceBackend core_backend(std::int32_t backend) {
    using llavon::ime::core::InferenceBackend;
    switch (backend) {
        case LLAVON_SETTINGS_BACKEND_CPU:
            return InferenceBackend::cpu;
        case LLAVON_SETTINGS_BACKEND_CUDA:
            return InferenceBackend::cuda;
        case LLAVON_SETTINGS_BACKEND_VULKAN:
            return InferenceBackend::vulkan;
        case LLAVON_SETTINGS_BACKEND_AUTO:
        default:
            return InferenceBackend::automatic;
    }
}

}  // namespace

void SettingsUiLoader::configure(
    const std::vector<llavon::ime::core::InferenceDeviceInfo>& devices,
    llavon::ime::core::InferenceDeviceSelection selected,
    const llavon::ime::core::InferenceRuntimeInfo& active,
    SaveInferenceSettings save_settings,
    std::vector<CustomNameSetting> custom_names,
    SaveCustomNames save_custom_names,
    bool shift_space_width_toggle_enabled,
    SaveWidthToggleSetting save_width_toggle) {
    devices_.clear();
    devices_.reserve(devices.size());
    for (const auto& device : devices) {
        if (device.backend != llavon::ime::core::InferenceBackend::cuda &&
            device.backend != llavon::ime::core::InferenceBackend::vulkan) {
            continue;
        }
        devices_.push_back(DeviceStorage{
            .backend = backend_value(device.backend),
            .device_type = device_type_value(device.type),
            .device_id = utf8_to_utf16(device.device_id),
            .name = utf8_to_utf16(device.name),
            .description = utf8_to_utf16(device.description),
            .memory_total = device.memory_total,
        });
    }
    active_device_ = DeviceStorage{
        .backend = backend_value(active.device.backend),
        .device_type = device_type_value(active.device.type),
        .device_id = utf8_to_utf16(active.device.device_id),
        .name = utf8_to_utf16(active.device.name),
        .description = utf8_to_utf16(active.device.description),
        .memory_total = active.device.memory_total,
    };
    gpu_offload_ = active.gpu_offload;
    fell_back_to_cpu_ = active.fell_back_to_cpu;
    selected_ = std::move(selected);
    save_settings_ = std::move(save_settings);
    custom_names_.clear();
    custom_names_.reserve(custom_names.size());
    for (const auto& custom_name : custom_names) {
        CustomNameStorage storage;
        storage.name = utf8::utf8to16(utf8::utf32to8(custom_name.name));
        storage.readings = custom_name.readings;
        custom_names_.push_back(std::move(storage));
    }
    save_custom_names_ = std::move(save_custom_names);
    shift_space_width_toggle_enabled_ = shift_space_width_toggle_enabled;
    save_width_toggle_ = std::move(save_width_toggle);
}

SettingsUiLoader::~SettingsUiLoader() {
    if (!module_) {
        return;
    }

    bool can_unload = true;
    if (started_ && stop_) {
        can_unload = stop_() == 0;
    }
    if (can_unload) {
        FreeLibrary(module_);
    }
}

bool SettingsUiLoader::show() {
    if (!load()) {
        report_error(L"The settings UI module could not be loaded.");
        return false;
    }
    if (!started_) {
        const std::int32_t result = start_();
        if (result != 0) {
            report_error(L"The settings UI thread could not be started.");
            return false;
        }
        started_ = true;
    }

    show_();
    return true;
}

bool SettingsUiLoader::load() {
    if (module_) {
        return true;
    }

    const std::filesystem::path directory = executable_directory();
    if (directory.empty()) {
        return false;
    }
    const std::filesystem::path module_path = directory / settings_ui_filename;
    module_ = LoadLibraryExW(module_path.c_str(), nullptr,
                             LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module_) {
        return false;
    }

    start_ = resolve<StartFunction>(module_, "llavon_settings_ui_start");
    configure_ = resolve<ConfigureFunction>(module_, "llavon_settings_ui_configure");
    show_ = resolve<ShowFunction>(module_, "llavon_settings_ui_show");
    stop_ = resolve<StopFunction>(module_, "llavon_settings_ui_stop");
    if (!configure_ || !start_ || !show_ || !stop_ || !configure_module()) {
        FreeLibrary(module_);
        module_ = nullptr;
        configure_ = nullptr;
        start_ = nullptr;
        show_ = nullptr;
        stop_ = nullptr;
        return false;
    }
    return true;
}

bool SettingsUiLoader::configure_module() {
    std::vector<llavon_settings_inference_device> devices;
    devices.reserve(devices_.size());
    for (const auto& device : devices_) {
        devices.push_back(llavon_settings_inference_device{
            .backend = device.backend,
            .device_type = device.device_type,
            .device_id = device.device_id.c_str(),
            .name = device.name.c_str(),
            .description = device.description.c_str(),
            .memory_total = device.memory_total,
        });
    }
    const llavon_settings_inference_device active_device{
        .backend = active_device_.backend,
        .device_type = active_device_.device_type,
        .device_id = active_device_.device_id.c_str(),
        .name = active_device_.name.c_str(),
        .description = active_device_.description.c_str(),
        .memory_total = active_device_.memory_total,
    };
    const std::u16string selected_device_id = utf8_to_utf16(selected_.device_id);

    std::vector<std::vector<const char16_t*>> custom_name_readings;
    custom_name_readings.reserve(custom_names_.size());
    std::vector<llavon_settings_custom_name> custom_names;
    custom_names.reserve(custom_names_.size());
    for (const auto& custom_name : custom_names_) {
        auto& readings = custom_name_readings.emplace_back();
        readings.reserve(custom_name.readings.size());
        for (const auto& reading : custom_name.readings) {
            readings.push_back(reading.c_str());
        }
        custom_names.push_back(llavon_settings_custom_name{
            .name = custom_name.name.c_str(),
            .readings = readings.data(),
            .reading_count = readings.size(),
        });
    }
    return configure_(
               devices.data(), devices.size(), backend_value(selected_.backend),
               selected_device_id.c_str(), &active_device, gpu_offload_ ? 1 : 0,
               fell_back_to_cpu_ ? 1 : 0, save_trampoline, this,
               custom_names.data(), custom_names.size(),
               save_custom_names_trampoline, this,
               shift_space_width_toggle_enabled_ ? 1 : 0,
               save_width_toggle_trampoline, this) == 0;
}

std::int32_t SettingsUiLoader::save_width_toggle_trampoline(
    void* context, std::int32_t enabled) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->save_width_toggle_) return ERROR_INVALID_FUNCTION;
    try {
        const bool value = enabled != 0;
        if (!self->save_width_toggle_(value)) return ERROR_WRITE_FAULT;
        self->shift_space_width_toggle_enabled_ = value;
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_WRITE_FAULT;
    }
}

std::int32_t SettingsUiLoader::save_custom_names_trampoline(
    void* context, const llavon_settings_custom_name* custom_names,
    std::size_t custom_name_count) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->save_custom_names_ ||
        (custom_name_count != 0 && !custom_names)) {
        return ERROR_INVALID_PARAMETER;
    }
    try {
        std::vector<CustomNameSetting> settings;
        settings.reserve(custom_name_count);
        for (std::size_t index = 0; index < custom_name_count; ++index) {
            const auto& source = custom_names[index];
            if (!source.name || (source.reading_count != 0 && !source.readings)) {
                return ERROR_INVALID_PARAMETER;
            }
            CustomNameSetting setting;
            setting.name = utf8::utf8to32(
                utf8::utf16to8(std::u16string_view(source.name)));
            setting.readings.reserve(source.reading_count);
            for (std::size_t reading = 0; reading < source.reading_count; ++reading) {
                if (!source.readings[reading]) return ERROR_INVALID_PARAMETER;
                setting.readings.emplace_back(source.readings[reading]);
            }
            settings.push_back(std::move(setting));
        }
        return self->save_custom_names_(settings) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    } catch (...) {
        return ERROR_WRITE_FAULT;
    }
}

std::int32_t SettingsUiLoader::save_trampoline(
    void* context, std::int32_t backend, const char16_t* device_id) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->save_settings_) {
        return ERROR_INVALID_FUNCTION;
    }
    try {
        llavon::ime::core::InferenceDeviceSelection selection;
        selection.backend = core_backend(backend);
        selection.device_id = utf8::utf16to8(device_id ? std::u16string_view(device_id)
                                                        : std::u16string_view{});
        if (selection.backend == llavon::ime::core::InferenceBackend::automatic ||
            selection.backend == llavon::ime::core::InferenceBackend::cpu) {
            selection.device_id.clear();
        }
        return self->save_settings_(selection) ? ERROR_SUCCESS : ERROR_WRITE_FAULT;
    } catch (...) {
        return ERROR_WRITE_FAULT;
    }
}

void SettingsUiLoader::report_error(const wchar_t* detail) const noexcept {
    const DWORD error = GetLastError();
    std::wstring message(detail);
    if (error != ERROR_SUCCESS) {
        message += L"\n\nWindows error: ";
        message += std::to_wstring(error);
    }
    MessageBoxW(nullptr, message.c_str(), L"Llavon IME", MB_OK | MB_ICONERROR);
}

}  // namespace llavon::service
