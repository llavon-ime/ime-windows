#pragma once

#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace llavon::service {

struct RawCommitInputEntry {
    std::u16string reading;
    std::u16string output;
    bool manually_selected = false;
};

struct RawCommitEvent {
    std::u16string context;
    std::u16string answer;
    std::vector<RawCommitInputEntry> input;
    std::string session_id;
    std::uint64_t sequence = 0;
    std::string committed_at_utc;
};

struct TrainingDataItem {
    std::u16string event_id;
    std::u16string context;
    std::u16string answer;
    std::u16string reading;
    bool revice = false;
};

struct TrainingDataRecord {
    std::u16string event_id;
    std::u16string context;
    std::u16string answer;
    std::string padding_json;
    bool revice = false;
};

struct LoraTrainingRun {
    std::int64_t id = 0;
    std::int64_t parent_id = 0;
    std::string base_model_revision;
    std::filesystem::path adapter_path;
    std::filesystem::path output_model_path;
    std::string completed_at_utc;
    std::size_t record_count = 0;
    std::size_t cumulative_record_count = 0;
    std::int64_t optimizer_steps = 0;
    std::int32_t rank = 0;
    double alpha = 0;
    double dropout = 0;
    std::u16string target_modules;
};

class TrainingDataWriter final {
public:
    explicit TrainingDataWriter(
        std::optional<std::filesystem::path> database_path = std::nullopt,
        std::chrono::steady_clock::duration staging_window =
            std::chrono::seconds(10));
    ~TrainingDataWriter();

    TrainingDataWriter(const TrainingDataWriter&) = delete;
    TrainingDataWriter& operator=(const TrainingDataWriter&) = delete;

    void enqueue(RawCommitEvent event);
    void discard_staged(std::string session_id, std::uint64_t sequence);
    struct ProtectionStatus { bool configured; bool enabled; };
    ProtectionStatus protection_status() const;
    void configure_password(std::string_view password);
    void set_recording_enabled(bool enabled);
    void reset_conversation_data();
    std::size_t pending_count() const noexcept {
        return pending_count_.load(std::memory_order_acquire);
    }
    void set_pending_count_callback(std::function<void(std::size_t)> callback);
    // Without a password, only IDs and selection metadata are returned.
    std::vector<TrainingDataItem> pending_items(std::string_view password = {}) const;
    std::vector<TrainingDataRecord> pending_records(
        const std::vector<std::u16string>& event_ids, std::string_view password = {}) const;
    bool delete_pending(std::u16string_view event_id) noexcept;
    bool exclude_unselected(
        const std::vector<std::u16string>& selected_event_ids,
        const std::vector<std::u16string>& reviewed_event_ids) noexcept;
    bool mark_trained(
        const std::vector<std::u16string>& trained_event_ids) noexcept;
    std::optional<LoraTrainingRun> latest_lora_training_run() const;
    std::vector<LoraTrainingRun> lora_training_history() const noexcept;
    bool complete_lora_training(
        const LoraTrainingRun& run,
        const std::vector<std::u16string>& trained_event_ids) noexcept;
    const std::filesystem::path& database_path() const noexcept {
        return database_path_;
    }

private:
    struct QueuedOperation {
        enum class Kind { commit, discard, clear } kind = Kind::commit;
        RawCommitEvent event;
        std::string session_id;
        std::uint64_t sequence = 0;
        std::chrono::steady_clock::time_point queued_at;
        std::uint64_t generation = 0;
    };

    void worker_main() noexcept;
    void publish_pending_delta(std::ptrdiff_t delta) noexcept;
    bool mark_records(const std::vector<std::u16string>& event_ids,
                      const char* state) noexcept;

    std::filesystem::path database_path_;
    mutable std::mutex protection_mutex_;
    std::atomic_bool recording_enabled_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<QueuedOperation> queue_;
    std::chrono::steady_clock::duration staging_window_;
    std::atomic<std::size_t> pending_count_{0};
    std::mutex callback_mutex_;
    std::function<void(std::size_t)> pending_count_callback_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace llavon::service
