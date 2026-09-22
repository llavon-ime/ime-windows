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

// Converts the raw frontend event to the validation-like JSONL schema. Keeping
// this function in the service is deliberate: the TSF DLL does not know about
// training-data versions, tone numbers, or correction metadata.
std::string serialize_training_data_event(const RawCommitEvent& event);

class TrainingDataWriter final {
public:
    explicit TrainingDataWriter(
        std::optional<std::filesystem::path> output_path = std::nullopt);
    ~TrainingDataWriter();

    TrainingDataWriter(const TrainingDataWriter&) = delete;
    TrainingDataWriter& operator=(const TrainingDataWriter&) = delete;

    void enqueue(RawCommitEvent event);

private:
    void worker_main() noexcept;

    std::optional<std::filesystem::path> output_path_;
    std::mutex mutex_;
    std::condition_variable available_;
    std::deque<RawCommitEvent> queue_;
    bool stopping_ = false;
    std::thread worker_;
};

}  // namespace llavon::service
