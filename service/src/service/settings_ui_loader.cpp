#include "settings_ui_loader.hpp"

#include "../settings/settings_ui_api.h"

#include <utf8/cpp20.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace llavon::service {
namespace {

struct TransientPassword {
    std::string value;
    explicit TransientPassword(const char16_t* password)
        : value(password ? utf8::utf16to8(std::u16string_view(password)) : std::string{}) {}
    ~TransientPassword() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
};

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
    CancelLora cancel_lora, ProtectionAction protection_action) {
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
    model_path_ = std::move(model_path);
    save_model_path_ = std::move(save_model_path);
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
    load_training_data_ = std::move(load_training_data);
    delete_training_data_ = std::move(delete_training_data);
    load_lora_history_ = std::move(load_lora_history);
    start_lora_training_ = std::move(start_lora_training);
    get_lora_status_ = std::move(get_lora_status);
    lora_model_action_ = std::move(lora_model_action);
    cancel_lora_ = std::move(cancel_lora);
    protection_action_ = std::move(protection_action);
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
        std::lock_guard lock(pending_count_mutex_);
        set_pending_count_ = nullptr;
        FreeLibrary(module_);
    }
}

void SettingsUiLoader::notify_pending_count(std::size_t count) noexcept {
    std::lock_guard lock(pending_count_mutex_);
    latest_pending_count_ = count;
    if (set_pending_count_) set_pending_count_(count);
}

bool SettingsUiLoader::show() {
    if (!started_ && load_training_data_) {
        refresh_training_items();
    }
    if (!start()) {
        return false;
    }

    show_();
    return true;
}

bool SettingsUiLoader::show_context_menu(POINT location) {
    if (!start()) {
        return false;
    }

    show_context_menu_(location.x, location.y);
    return true;
}

bool SettingsUiLoader::start() {
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
    configure_ = resolve<ConfigureFunction>(module_, "llavon_settings_ui_configure_v4");
    show_ = resolve<ShowFunction>(module_, "llavon_settings_ui_show");
    show_context_menu_ =
        resolve<ShowContextMenuFunction>(module_, "llavon_settings_ui_show_context_menu");
    stop_ = resolve<StopFunction>(module_, "llavon_settings_ui_stop");
    const auto set_pending_count = resolve<SetPendingCountFunction>(
        module_, "llavon_settings_ui_set_pending_count");
    if (!configure_ || !start_ || !show_ || !show_context_menu_ || !stop_ ||
        !set_pending_count ||
        !configure_module()) {
        FreeLibrary(module_);
        module_ = nullptr;
        configure_ = nullptr;
        start_ = nullptr;
        show_ = nullptr;
        show_context_menu_ = nullptr;
        stop_ = nullptr;
        return false;
    }
    {
        std::lock_guard lock(pending_count_mutex_);
        set_pending_count_ = set_pending_count;
        if (latest_pending_count_) set_pending_count_(*latest_pending_count_);
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

    std::vector<llavon_settings_training_item> training_items;
    training_items.reserve(training_items_.size());
    for (const auto& item : training_items_) {
        training_items.push_back(llavon_settings_training_item{
            .event_id = item.event_id.c_str(),
            .context = item.context.c_str(),
            .answer = item.answer.c_str(),
            .reading = item.reading.c_str(),
            .revice = item.revice ? 1 : 0,
        });
    }
    return configure_(
               devices.data(), devices.size(), backend_value(selected_.backend),
               selected_device_id.c_str(), &active_device, gpu_offload_ ? 1 : 0,
               fell_back_to_cpu_ ? 1 : 0, save_trampoline, this,
               model_path_.c_str(), save_model_path_trampoline, this,
               custom_names.data(), custom_names.size(),
               save_custom_names_trampoline, this,
               shift_space_width_toggle_enabled_ ? 1 : 0,
               save_width_toggle_trampoline, this,
               training_items.data(), training_items.size(),
               refresh_training_items_trampoline, this,
               delete_training_item_trampoline, this,
               get_lora_history_trampoline, this,
               start_lora_training_trampoline, this,
               get_lora_status_trampoline, this,
               lora_model_action_trampoline, this,
               cancel_lora_trampoline, this, protection_trampoline, this) == 0;
}

std::int32_t SettingsUiLoader::protection_trampoline(
    void* context, std::int32_t action, const char16_t* password, std::size_t* result) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !result) return ERROR_INVALID_PARAMETER;
    try {
        if (action == LLAVON_PROTECTION_CLEAR_VIEW) {
            self->training_items_.clear();
            *result = 0;
        } else {
            TransientPassword secret(password);
            *result = self->protection_action_(action, secret.value);
            if (action == LLAVON_PROTECTION_RESET) {
                self->training_items_.clear();
            }
        }
        return ERROR_SUCCESS;
    } catch (...) { return ERROR_ACCESS_DENIED; }
}

std::int32_t SettingsUiLoader::get_lora_history_trampoline(
    void* context, llavon_settings_lora_history_item* items,
    std::size_t item_capacity, std::size_t* item_count) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !item_count || (item_capacity != 0 && !items)) {
        return ERROR_INVALID_PARAMETER;
    }
    try {
        self->lora_history_.clear();
        if (self->load_lora_history_) {
            for (const auto& run : self->load_lora_history_()) {
                self->lora_history_.push_back(LoraHistoryStorage{
                    .id = run.id,
                    .parent_id = run.parent_id,
                    .completed_at_utc = utf8::utf8to16(run.completed_at_utc),
                    .output_model_path = run.output_model_path.u16string(),
                    .record_count = run.record_count,
                    .cumulative_record_count = run.cumulative_record_count,
                    .optimizer_steps = run.optimizer_steps,
                    .rank = run.rank,
                    .alpha = run.alpha,
                    .dropout = run.dropout,
                    .target_modules = run.target_modules,
                });
            }
        }
        *item_count = self->lora_history_.size();
        if (!items) return ERROR_SUCCESS;
        if (item_capacity < self->lora_history_.size()) return ERROR_INSUFFICIENT_BUFFER;
        for (std::size_t index = 0; index < self->lora_history_.size(); ++index) {
            const auto& source = self->lora_history_[index];
            items[index] = llavon_settings_lora_history_item{
                .id = source.id,
                .parent_id = source.parent_id,
                .completed_at_utc = source.completed_at_utc.c_str(),
                .output_model_path = source.output_model_path.c_str(),
                .record_count = source.record_count,
                .cumulative_record_count = source.cumulative_record_count,
                .optimizer_steps = source.optimizer_steps,
                .rank = source.rank,
                .alpha = source.alpha,
                .dropout = source.dropout,
                .target_modules = source.target_modules.c_str(),
            };
        }
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

void SettingsUiLoader::refresh_training_items(
    std::string_view password, std::int64_t base_run_id) {
    training_items_.clear();
    if (!load_training_data_) return;
    for (auto& item : load_training_data_(password, base_run_id)) {
        training_items_.push_back(TrainingDataStorage{
            .event_id = std::move(item.event_id),
            .context = std::move(item.context),
            .answer = std::move(item.answer),
            .reading = std::move(item.reading),
            .revice = item.revice,
        });
    }
}

std::int32_t SettingsUiLoader::refresh_training_items_trampoline(
    void* context, llavon_settings_training_item* items,
    std::size_t item_capacity, std::size_t* item_count, const char16_t* password,
    std::int64_t base_run_id) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !item_count || (item_capacity != 0 && !items)) {
        return ERROR_INVALID_PARAMETER;
    }
    try {
        TransientPassword secret(password);
        if (!items) self->refresh_training_items(secret.value, base_run_id);
        *item_count = self->training_items_.size();
        if (!items) return ERROR_SUCCESS;
        if (item_capacity < self->training_items_.size()) return ERROR_INSUFFICIENT_BUFFER;
        for (std::size_t index = 0; index < self->training_items_.size(); ++index) {
            const auto& source = self->training_items_[index];
            items[index] = llavon_settings_training_item{
                .event_id = source.event_id.c_str(),
                .context = source.context.c_str(),
                .answer = source.answer.c_str(),
                .reading = source.reading.c_str(),
                .revice = source.revice ? 1 : 0,
            };
        }
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

std::int32_t SettingsUiLoader::delete_training_item_trampoline(
    void* context, const char16_t* event_id) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->delete_training_data_ || !event_id) {
        return ERROR_INVALID_PARAMETER;
    }
    try {
        if (!self->delete_training_data_(event_id)) return ERROR_WRITE_FAULT;
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_WRITE_FAULT;
    }
}

std::int32_t SettingsUiLoader::start_lora_training_trampoline(
    void* context, const char16_t* const* selected_event_ids,
    std::size_t selected_event_id_count,
    const llavon_settings_lora_options* options, const char16_t* password) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->start_lora_training_ || !options ||
        (selected_event_id_count != 0 && !selected_event_ids)) {
        return ERROR_INVALID_PARAMETER;
    }
    try {
        std::vector<std::u16string> selected;
        selected.reserve(selected_event_id_count);
        for (std::size_t index = 0; index < selected_event_id_count; ++index) {
            if (!selected_event_ids[index]) return ERROR_INVALID_PARAMETER;
            selected.emplace_back(selected_event_ids[index]);
        }
        TransientPassword secret(password);
        return self->start_lora_training_(selected, *options, secret.value)
            ? ERROR_SUCCESS
            : ERROR_WRITE_FAULT;
    } catch (...) {
        return ERROR_WRITE_FAULT;
    }
}

std::int32_t SettingsUiLoader::get_lora_status_trampoline(
    void* context, llavon_settings_lora_status* status) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->get_lora_status_ || !status) return ERROR_INVALID_PARAMETER;
    try {
        const auto current = self->get_lora_status_();
        self->lora_status_message_ = current.message;
        self->lora_status_revision_ = current.model_revision;
        self->lora_status_output_path_ = current.output_model_path;
        self->lora_trainer_version_ = current.trainer_version;
        self->lora_trainer_backend_ = current.trainer_backend;
        self->lora_trainer_release_version_ = current.trainer_release_version;
        self->lora_trainer_message_ = current.trainer_message;
        *status = llavon_settings_lora_status{
            .stage = static_cast<std::int32_t>(current.stage),
            .progress = current.progress,
            .model_available = current.model_available ? 1 : 0,
            .model_update_available = current.model_update_available ? 1 : 0,
            .message = self->lora_status_message_.c_str(),
            .model_revision = self->lora_status_revision_.c_str(),
            .output_model_path = self->lora_status_output_path_.c_str(),
            .trainer_available = current.trainer_available ? 1 : 0,
            .trainer_assets = current.trainer_assets,
            .trainer_version = self->lora_trainer_version_.c_str(),
            .trainer_backend = self->lora_trainer_backend_.c_str(),
            .trainer_release_version = self->lora_trainer_release_version_.c_str(),
            .trainer_message = self->lora_trainer_message_.c_str(),
        };
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

std::int32_t SettingsUiLoader::lora_model_action_trampoline(
    void* context, std::int32_t action) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->lora_model_action_) return ERROR_INVALID_FUNCTION;
    try {
        return self->lora_model_action_(action)
            ? ERROR_SUCCESS
            : ERROR_BUSY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
}

void SettingsUiLoader::cancel_lora_trampoline(void* context) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->cancel_lora_) return;
    try {
        self->cancel_lora_();
    } catch (...) {
    }
}

std::int32_t SettingsUiLoader::save_model_path_trampoline(
    void* context, const char16_t* model_path) noexcept {
    auto* self = static_cast<SettingsUiLoader*>(context);
    if (!self || !self->save_model_path_ || !model_path) {
        return ERROR_INVALID_PARAMETER;
    }
    try {
        const std::filesystem::path path{std::u16string_view(model_path)};
        if (!self->save_model_path_(path)) return ERROR_WRITE_FAULT;
        self->model_path_ = path.u16string();
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_WRITE_FAULT;
    }
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
