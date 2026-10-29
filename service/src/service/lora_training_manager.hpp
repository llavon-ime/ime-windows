#pragma once

#include "training_data_writer.hpp"
#include "lora_types.hpp"
#include "winrt_http.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace llavon::service {

class LoraTrainingManager final {
public:
    LoraTrainingManager(std::shared_ptr<TrainingDataWriter> training_data,
                        std::filesystem::path tables_directory);
    ~LoraTrainingManager();

    LoraTrainingManager(const LoraTrainingManager&) = delete;
    LoraTrainingManager& operator=(const LoraTrainingManager&) = delete;

    LoraOperationStatus status();
    bool check_updates_async();
    bool download_model_async();
    bool install_trainer_async();
    bool install_cuda_runtime_async();
    bool remove_cuda_runtime_async();
    bool start_training_async(std::vector<std::u16string> event_ids,
                              LoraTrainingOptions options, std::string_view password);
    void on_model_applied(const std::filesystem::path& model_path) const noexcept;
    bool ensure_model_exported(const std::filesystem::path& model_path);
    bool discard_model_export(const std::filesystem::path& model_path);
    std::size_t discard_plaintext_datasets();
    void reset_conversation_data();
    void cancel() noexcept;

private:
    using Operation = void (LoraTrainingManager::*)();

    bool launch(Operation operation);
    void check_updates_worker();
    void download_model_worker();
    void install_trainer_worker();
    void install_cuda_runtime_worker();
    void remove_cuda_runtime_worker();
    void refresh_installed_trainer();
    // Asks the installed trainer; runs a child process, so only call it from
    // a worker, never from status().
    void refresh_cuda_runtime();
    void finish_trainer_operation(std::u16string message,
                                  std::u16string trainer_warning = {});
    void training_worker();
    void prune_obsolete_gguf_models(
        const std::filesystem::path& applied_model_path) const noexcept;
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
                    bool parse_training_progress,
                    std::string* captured_output = nullptr,
                    const std::function<void(std::string_view)>& on_line = {});

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
    std::vector<std::u16string> pending_event_ids_;
    std::vector<TrainingDataRecord> pending_records_;
    LoraTrainingOptions pending_options_;
    std::optional<LoraTrainingRun> pending_base_run_;
};

}  // namespace llavon::service
