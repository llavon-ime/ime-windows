#include "service/training_data_writer.hpp"
#include "service/lora_dataset_builder.hpp"

#include <windows.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>

using llavon::service::RawCommitEvent;
using llavon::service::RawCommitInputEntry;
using llavon::service::TrainingDataWriter;
using llavon::service::write_lora_numeric_dataset;

int main() {
    const RawCommitEvent event{
        .context = u"sample",
        .answer = u"result",
        .input = {
            RawCommitInputEntry{
                .reading = u"\u3105\u02c7",
                .output = u"a",
                .manually_selected = true,
            },
            RawCommitInputEntry{.reading = u"\u3106 ", .output = u"b"},
            RawCommitInputEntry{.output = u","},
        },
        .session_id = "session",
        .sequence = 7,
        .committed_at_utc = "2026-09-22T12:34:56.789Z",
    };
    const RawCommitEvent escaped{
        .context = u"a\n\"b",
        .answer = u"x",
        .input = {RawCommitInputEntry{.reading = u"\u3105", .output = u"x"}},
        .session_id = "s",
        .sequence = 1,
        .committed_at_utc = "t",
    };
    const RawCommitEvent trainable{
        .answer = u"你",
        .input = {RawCommitInputEntry{
            .reading = u"\u310b\u3127\u02c7",
            .output = u"你",
        }},
        .session_id = "train",
        .sequence = 1,
        .committed_at_utc = "t",
    };
    const RawCommitEvent arrived_after_review{
        .answer = u"late",
        .input = {RawCommitInputEntry{.reading = u"later", .output = u"late"}},
        .session_id = "late",
        .sequence = 1,
        .committed_at_utc = "t",
    };
    std::wstring temporary_directory(MAX_PATH, L'\0');
    const DWORD temporary_length = GetTempPathW(
        static_cast<DWORD>(temporary_directory.size()), temporary_directory.data());
    if (temporary_length == 0 || temporary_length >= temporary_directory.size()) return 3;
    temporary_directory.resize(temporary_length);
    const auto path = std::filesystem::path(temporary_directory) /
                       (L"llavon-training-data-writer-test-" +
                       std::to_wstring(GetCurrentProcessId()) + L".sqlite3");
    DeleteFileW(path.c_str());
    {
        TrainingDataWriter writer(path);
        writer.enqueue(event);
        writer.enqueue(escaped);
        writer.enqueue(trainable);
        writer.enqueue(arrived_after_review);
    }

    {
        TrainingDataWriter writer(path);
        const auto pending = writer.pending_items();
        if (pending.size() != 4 || pending[0].event_id != u"session:7" ||
            pending[0].context != u"sample" || pending[0].answer != u"result" ||
            pending[0].reading.empty() || !pending[0].revice) {
            return 4;
        }
        const auto stored = writer.pending_records({u"session:7", u"s:1"});
        if (stored.size() != 2 || !stored[0].revice || stored[1].revice ||
            stored[0].padding_json.find(R"("tone":3)") == std::string::npos ||
            stored[0].padding_json.find(R"("tone":1)") == std::string::npos ||
            stored[0].padding_json.find(R"({"literal":",")") == std::string::npos ||
            stored[1].padding_json.find(R"({"rawReading":)") == std::string::npos) {
            return 5;
        }
        const auto records = writer.pending_records({u"train:1"});
        if (records.size() != 1 || records[0].padding_json.empty()) return 6;

        auto config_path = path;
        config_path += L".config.json";
        auto dataset_path = path;
        dataset_path += L".training.jsonl";
        {
            std::ofstream config(config_path, std::ios::binary | std::ios::trunc);
            config << R"({"vocab_size":20000,"max_position_embeddings":384})";
        }
        const auto dataset = write_lora_numeric_dataset(
            records, std::filesystem::path(LLAVON_TEST_TABLES_DIR),
            config_path, dataset_path, 384);
        std::ifstream dataset_input(dataset_path, std::ios::binary);
        const std::string numeric_row{
            std::istreambuf_iterator<char>(dataset_input),
            std::istreambuf_iterator<char>()};
        DeleteFileW(config_path.c_str());
        DeleteFileW(dataset_path.c_str());
        if (dataset.written != 1 || dataset.pad_token_id != 0 ||
            numeric_row.find(R"("candidate_masks")") == std::string::npos) {
            return 7;
        }
        if (!writer.exclude_unselected(
                {u"session:7"}, {u"session:7", u"s:1", u"train:1"})) return 8;
        const auto remaining = writer.pending_items();
        if (remaining.size() != 2 || remaining[0].event_id != u"session:7" ||
            remaining[1].event_id != u"late:1") return 9;
        if (!writer.mark_trained({u"session:7"}) ||
            writer.pending_items().size() != 1) return 10;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open16(path.c_str(), &database) != SQLITE_OK) return 11;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            database,
            "SELECT training_state, COUNT(*) FROM training_commits "
            "GROUP BY training_state ORDER BY training_state",
            -1, &statement, nullptr) != SQLITE_OK) {
        sqlite3_close(database);
        return 12;
    }
    bool saw_excluded = false;
    bool saw_trained = false;
    bool saw_pending = false;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const std::string state = reinterpret_cast<const char*>(
            sqlite3_column_text(statement, 0));
        const int count = sqlite3_column_int(statement, 1);
        saw_excluded = saw_excluded || (state == "excluded" && count == 2);
        saw_trained = saw_trained || (state == "trained" && count == 1);
        saw_pending = saw_pending || (state == "pending" && count == 1);
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    DeleteFileW(path.c_str());
    auto wal_path = path;
    wal_path += L"-wal";
    DeleteFileW(wal_path.c_str());
    auto shm_path = path;
    shm_path += L"-shm";
    DeleteFileW(shm_path.c_str());
    if (!saw_excluded || !saw_trained || !saw_pending) return 13;
    return 0;
}
