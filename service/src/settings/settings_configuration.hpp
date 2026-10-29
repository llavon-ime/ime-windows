#pragma once

#include "../service/lora_types.hpp"
#include "../service/training_data_writer.hpp"

#include <ime-core/core.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace llavon::settings {

using InferenceBackend = ime::core::InferenceBackend;
using InferenceDeviceType = ime::core::InferenceDeviceType;
using TrainingDataOption = service::TrainingDataItem;
using LoraTrainingOptions = service::LoraTrainingOptions;
using LoraOperationStatus = service::LoraOperationStatus;
using LoraOperationStage = service::LoraOperationStage;
using CudaRuntimeState = service::CudaRuntimeState;

enum class ModelPreparation { prepare, discard };
enum class LoraAction {
    check_updates,
    download_model,
    install_trainer,
    install_cuda_runtime,
    remove_cuda_runtime
};
struct ProtectionStatus {
    bool configured = false;
    bool enabled = false;
};

struct ProtectionActions {
    std::function<std::optional<ProtectionStatus>()> status;
    std::function<bool(std::u16string_view)> setup;
    std::function<bool(bool)> set_enabled;
    std::function<std::optional<std::size_t>()> cleanup;
    std::function<bool()> reset;
};

struct InferenceDeviceOption {
    InferenceBackend backend = InferenceBackend::automatic;
    InferenceDeviceType device_type = InferenceDeviceType::cpu;
    std::u16string device_id;
    std::u16string name;
    std::u16string description;
    std::uint64_t memory_total = 0;
};

struct CustomNameOption {
    std::u16string name;
    std::vector<std::u16string> readings;
};

// Operations run on their caller's thread. The UI explicitly schedules blocking
// operations on workers. Returned snapshots own every string and collection.
struct SettingsActions {
    std::function<bool(InferenceBackend, std::u16string_view)> save_inference;
    std::function<bool(const std::filesystem::path&)> save_model_path;
    std::function<bool(const std::filesystem::path&, ModelPreparation)> prepare_model;
    std::function<bool(const std::vector<CustomNameOption>&)> save_custom_names;
    std::function<bool(bool)> save_width_toggle;
    std::function<bool(bool)> save_immediate_conversion;
    std::function<bool(bool)> save_gpu_boost;
    std::function<bool(bool)> save_update_notifications;
    std::function<std::optional<std::vector<TrainingDataOption>>(std::u16string_view, std::int64_t)>
        load_training_items;
    std::function<bool(std::u16string_view)> delete_training_item;
    std::function<std::vector<service::LoraTrainingRun>()> load_lora_history;
    std::function<std::vector<service::LoraBaseModel>()> load_lora_base_models;
    std::function<bool(
        const std::vector<std::u16string>&, LoraTrainingOptions, std::u16string_view)>
        start_lora_training;
    std::function<std::optional<LoraOperationStatus>()> get_lora_status;
    std::function<bool(LoraAction)> lora_model_action;
    std::function<void()> cancel_lora;
    ProtectionActions protection;
};

struct SettingsConfiguration {
    std::vector<InferenceDeviceOption> devices;
    InferenceBackend selected_backend = InferenceBackend::automatic;
    std::u16string selected_device_id;
    InferenceDeviceOption active_device;
    bool gpu_offload = false;
    bool fell_back_to_cpu = false;
    std::u16string model_path;
    std::u16string base_model_path;
    std::vector<CustomNameOption> custom_names;
    bool shift_space_width_toggle_enabled = false;
    bool immediate_conversion_enabled = false;
    bool gpu_boost_enabled = true;
    bool major_update_notifications_enabled = true;
    std::vector<TrainingDataOption> training_items;
    SettingsActions actions;
};

} // namespace llavon::settings
