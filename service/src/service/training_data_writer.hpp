#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
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

class TrainingDataWriter final {
public:
    explicit TrainingDataWriter(
        std::optional<std::filesystem::path> database_path = std::nullopt);
    ~TrainingDataWriter();

    TrainingDataWriter(const TrainingDataWriter&) = delete;
    TrainingDataWriter& operator=(const TrainingDataWriter&) = delete;

    void enqueue(RawCommitEvent event);
    std::vector<TrainingDataItem> pending_items() const noexcept;
    std::vector<TrainingDataRecord> pending_records(
        const std::vector<std::u16string>& event_ids) const;
    bool exclude_unselected(
        const std::vector<std::u16string>& selected_event_ids,
        const std::vector<std::u16string>& reviewed_event_ids) noexcept;
    bool mark_trained(
        const std::vector<std::u16string>& trained_event_ids) noexcept;
    const std::filesystem::path& database_path() const noexcept {
        return database_path_;
    }

private:
    void worker_main() noexcept;
    bool mark_records(const std::vector<std::u16string>& event_ids,
                      const char* state) noexcept;

    std::filesystem::path database_path_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<RawCommitEvent> queue_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace llavon::service
