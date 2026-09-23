#pragma once

#include "training_data_writer.hpp"
#include "winrt_http.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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
};

struct LoraTrainingOptions {
    std::int32_t rank = 8;
    double alpha = 16;
    double dropout = 0;
    std::int32_t batch_size = 1;
    std::int32_t gradient_accumulation = 1;
    std::int32_t epochs = 210;
    std::int32_t max_steps = 210;
    double learning_rate = 1e-4;
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
};

struct LoraOperationStatus {
    LoraOperationStage stage = LoraOperationStage::idle;
    double progress = 0;
    std::u16string message;
    bool model_available = false;
    bool model_update_available = false;
    std::u16string model_revision;
    std::u16string output_model_path;
};

class LoraTrainingManager final {
public:
    LoraTrainingManager(std::shared_ptr<TrainingDataWriter> training_data,
                        std::filesystem::path tables_directory);
    ~LoraTrainingManager();

    LoraTrainingManager(const LoraTrainingManager&) = delete;
    LoraTrainingManager& operator=(const LoraTrainingManager&) = delete;

    LoraOperationStatus status() const;
    bool check_model_async();
    bool download_model_async();
    bool start_training_async(std::vector<std::u16string> event_ids,
                              const std::vector<std::u16string>& reviewed_event_ids,
                              LoraTrainingOptions options);
    void cancel() noexcept;

private:
    using Operation = void (LoraTrainingManager::*)();

    bool launch(Operation operation);
    void check_model_worker();
    void download_model_worker();
    void training_worker();
    void set_status(LoraOperationStage stage, double progress,
                    std::u16string message);
    void set_failed(const std::exception& error) noexcept;
    void set_failed_unknown() noexcept;
    std::string resolve_remote_revision();
    bool installed_model_is_complete(std::string* revision = nullptr) const;
    void download_asset(const std::string& revision, std::string_view filename,
                        double progress_start, double progress_end);
    int run_process(const std::filesystem::path& executable,
                    const std::vector<std::wstring>& arguments,
                    bool parse_training_progress);

    std::shared_ptr<TrainingDataWriter> training_data_;
    std::filesystem::path tables_directory_;
    std::filesystem::path assets_root_;
    mutable std::mutex status_mutex_;
    LoraOperationStatus status_;
    std::mutex operation_mutex_;
    std::thread worker_;
    std::atomic_bool busy_{false};
    std::atomic_bool cancelling_{false};
    WinrtHttpTransfer http_transfer_;
    std::mutex process_mutex_;
    HANDLE active_process_ = nullptr;
    std::vector<std::u16string> pending_event_ids_;
    LoraTrainingOptions pending_options_;
};

}  // namespace llavon::service
