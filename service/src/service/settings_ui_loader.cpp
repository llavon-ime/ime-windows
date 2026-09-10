#include "settings_ui_loader.hpp"

#include "../settings/settings_ui_api.h"

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

std::wstring utf8_to_wide(std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required);
    return result;
}

std::string wide_to_utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
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
    SaveInferenceSettings save_settings) {
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
            .device_id = utf8_to_wide(device.device_id),
            .name = utf8_to_wide(device.name),
            .description = utf8_to_wide(device.description),
            .memory_total = device.memory_total,
        });
    }
    active_device_ = DeviceStorage{
        .backend = backend_value(active.device.backend),
        .device_type = device_type_value(active.device.type),
        .device_id = utf8_to_wide(active.device.device_id),
        .name = utf8_to_wide(active.device.name),
        .description = utf8_to_wide(active.device.description),
        .memory_total = active.device.memory_total,
    };
    gpu_offload_ = active.gpu_offload;
    fell_back_to_cpu_ = active.fell_back_to_cpu;
    selected_ = std::move(selected);
    save_settings_ = std::move(save_settings);
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
    const std::wstring selected_device_id = utf8_to_wide(selected_.device_id);
    return configure_(
               devices.data(), devices.size(), backend_value(selected_.backend),
               selected_device_id.c_str(), &active_device, gpu_offload_ ? 1 : 0,
               fell_back_to_cpu_ ? 1 : 0, save_trampoline, this) == 0;
}

std::int32_t SettingsUiLoader::save_trampoline(
    void* context, std::int32_t backend, const wchar_t* device_id) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->save_settings_) {
        return ERROR_INVALID_FUNCTION;
    }
    try {
        llavon::ime::core::InferenceDeviceSelection selection;
        selection.backend = core_backend(backend);
        selection.device_id = wide_to_utf8(device_id ? device_id : L"");
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
