#pragma once

#include "lora_training_presets.hpp"

#include <cstdint>
#include <string>

namespace llavon::service {

enum class LoraOperationStage : std::int32_t {
    idle = 0,
    checking_model = 1,
    downloading_model = 2,
    model_ready = 3,
    preparing_data = 4,
    training = 5,
    exporting_model = 6,
    completed = 7,
    failed = 8,
    cancelled = 9,
    checking_trainer = 10,
    installing_trainer = 11,
    checking_updates = 12,
};

enum class CudaRuntimeState : std::int32_t {
    unknown = 0,
    not_installed = 1,
    installed = 2,
    unsupported = 3,
};

struct LoraTrainingOptions {
    std::int64_t base_run_id = 0;
    std::string base_model_fingerprint;
    std::string base_model_revision;
    std::int32_t rank = 8;
    double alpha = 16;
    double dropout = 0;
    std::int32_t batch_size = 1;
    std::int32_t gradient_accumulation = 1;
    std::int32_t epochs = lora_training_preset(LoraTrainingStrength::low).epochs;
    std::int32_t max_steps = -1;
    double learning_rate = lora_training_preset(LoraTrainingStrength::low).learning_rate;
    double weight_decay = 0;
    std::int32_t warmup_steps = 0;
    double max_gradient_norm = 1;
    std::int32_t save_every = 0;
    std::int32_t device = 0;
    std::int32_t seed = 42;
    bool shuffle = true;
    std::int32_t max_sequence_length = 384;
    std::u16string dtype = u"float32";
    std::u16string target_modules = u"q_proj,v_proj";
    LoraTrainingStrength strength = LoraTrainingStrength::low;
    bool only_manually_selected = true;
    bool train_until_remembered = false;
    bool only_train_incorrect = false;
    bool stabilize_intruders = true;
};

struct LoraOperationStatus {
    LoraOperationStage stage = LoraOperationStage::idle;
    double progress = 0;
    std::u16string message;
    bool model_available = false;
    bool model_update_available = false;
    std::u16string model_revision;
    std::string base_model_fingerprint;
    std::u16string base_model_output_path;
    std::u16string output_model_path;
    bool trainer_available = false;
    CudaRuntimeState cuda_runtime = CudaRuntimeState::unknown;
    std::u16string trainer_version;
    std::u16string trainer_backend;
    std::u16string trainer_commit;
    std::u16string trainer_release_version;
    bool trainer_update_available = false;
    std::u16string trainer_message;
};

} // namespace llavon::service
