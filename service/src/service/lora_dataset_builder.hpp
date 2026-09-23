#pragma once

#include "training_data_writer.hpp"

#include <cstdint>
#include <filesystem>
#include <vector>

namespace llavon::service {

struct LoraDatasetBuildResult {
    std::size_t written = 0;
    std::size_t skipped = 0;
    std::int64_t pad_token_id = 0;
    std::int64_t vocabulary_size = 0;
    std::int64_t model_max_sequence_length = 0;
    std::vector<std::u16string> included_event_ids;
};

LoraDatasetBuildResult write_lora_numeric_dataset(
    const std::vector<TrainingDataRecord>& records,
    const std::filesystem::path& tables_directory,
    const std::filesystem::path& model_config_path,
    const std::filesystem::path& destination,
    std::int32_t maximum_sequence_length);

}  // namespace llavon::service
