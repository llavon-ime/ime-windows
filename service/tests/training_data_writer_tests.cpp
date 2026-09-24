#include "service/training_data_writer.hpp"
#include "service/lora_dataset_builder.hpp"

#include <windows.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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
        .input = {RawCommitInputEntry{.reading = u"\u310c\u311e ", .output = u"late"}},
        .session_id = "late",
        .sequence = 1,
        .committed_at_utc = "t",
    };
    const RawCommitEvent literal_only{
        .answer = u"。ㄅ",
        .input = {
            RawCommitInputEntry{.output = u"。"},
            RawCommitInputEntry{.output = u"ㄅ"},
        },
        .session_id = "literal",
        .sequence = 1,
        .committed_at_utc = "t",
    };
    const RawCommitEvent non_bopomofo_reading{
        .answer = u"x",
        .input = {RawCommitInputEntry{.reading = u"abc", .output = u"x"}},
        .session_id = "latin",
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
    std::vector<std::size_t> inserted_counts;
    {
        TrainingDataWriter writer(path);
        writer.set_pending_count_callback([&](std::size_t count) {
            inserted_counts.push_back(count);
        });
        writer.enqueue(event);
        writer.enqueue(escaped);
        writer.enqueue(trainable);
        writer.enqueue(arrived_after_review);
        writer.enqueue(event); // Duplicate IDs must not increment the count.
        writer.enqueue(literal_only);
        writer.enqueue(non_bopomofo_reading);
    }
    if (inserted_counts != std::vector<std::size_t>{0, 1, 2, 3, 4}) return 14;

    // Records created by an older collector must also disappear from the list.
    sqlite3* legacy_database = nullptr;
    if (sqlite3_open16(path.c_str(), &legacy_database) != SQLITE_OK) return 18;
    const char* legacy_insert =
        "INSERT INTO training_commits(event_id,context,answer,padding_json,reading,"
        "revice,committed_at_utc) VALUES "
        "('legacy:1','','ㄅ','[{\"literal\":\"ㄅ\"}]','ㄅ',0,'t')";
    const int legacy_result = sqlite3_exec(
        legacy_database, legacy_insert, nullptr, nullptr, nullptr);
    sqlite3_close(legacy_database);
    if (legacy_result != SQLITE_OK) return 19;

    {
        TrainingDataWriter writer(path);
        std::vector<std::size_t> changed_counts;
        writer.set_pending_count_callback([&](std::size_t count) {
            changed_counts.push_back(count);
        });
        if (writer.pending_count() != 4 ||
            changed_counts != std::vector<std::size_t>{4}) return 15;
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
        dataset_input.close();
        if (dataset.written != 1 || dataset.pad_token_id != 0 ||
            numeric_row.find(R"("candidate_masks")") == std::string::npos) {
            return 7;
        }
        auto selected_record = records.front();
        selected_record.event_id = u"selected:1";
        selected_record.context = u"a";
        selected_record.revice = true;
        const auto weighted = write_lora_numeric_dataset(
            {records.front(), selected_record},
            std::filesystem::path(LLAVON_TEST_TABLES_DIR),
            config_path, dataset_path, 384);
        std::ifstream weighted_input(dataset_path, std::ios::binary);
        std::string line;
        std::string selected_row;
        std::size_t normal_count = 0;
        std::size_t selected_count = 0;
        while (std::getline(weighted_input, line)) {
            if (line == numeric_row.substr(0, numeric_row.size() - 1)) {
                ++normal_count;
            } else if (selected_row.empty() || line == selected_row) {
                selected_row = line;
                ++selected_count;
            } else {
                return 23;
            }
        }
        weighted_input.close();
        DeleteFileW(config_path.c_str());
        DeleteFileW(dataset_path.c_str());
        if (weighted.written != 2 || weighted.skipped != 0 ||
            weighted.included_event_ids !=
                std::vector<std::u16string>{u"train:1", u"selected:1"} ||
            normal_count != 1 || selected_count != 3 || selected_row.empty()) {
            return 23;
        }
        if (!writer.exclude_unselected(
                {u"session:7"}, {u"session:7", u"s:1", u"train:1"})) return 8;
        if (writer.pending_count() != 2 ||
            changed_counts != std::vector<std::size_t>{4, 2}) return 16;
        const auto remaining = writer.pending_items();
        if (remaining.size() != 2 || remaining[0].event_id != u"session:7" ||
            remaining[1].event_id != u"late:1") return 9;
        if (!writer.mark_trained({u"session:7"}) ||
            writer.pending_items().size() != 1) return 10;
        if (writer.pending_count() != 1 ||
            changed_counts != std::vector<std::size_t>{4, 2, 1}) return 17;
        const llavon::service::LoraTrainingRun run{
            .base_model_revision = "0123456789012345678901234567890123456789",
            .adapter_path = path.parent_path() / L"adapter",
            .output_model_path = path.parent_path() / L"personalized.gguf",
            .completed_at_utc = "2026-09-24T01:02:03.004Z",
            .record_count = 1,
            .cumulative_record_count = 1,
            .optimizer_steps = 5,
            .rank = 8,
            .alpha = 16,
            .target_modules = u"q_proj,v_proj",
        };
        if (!writer.complete_lora_training(run, {u"late:1"})) return 20;
        const auto history = writer.lora_training_history();
        const auto latest = writer.latest_lora_training_run();
        if (history.size() != 1 || !latest || latest->id != history[0].id ||
            latest->record_count != 1 || latest->cumulative_record_count != 1 ||
            latest->optimizer_steps != 5 || latest->rank != 8 ||
            latest->target_modules != u"q_proj,v_proj") return 21;
        if (writer.pending_count() != 0 ||
            changed_counts != std::vector<std::size_t>{4, 2, 1, 0}) return 22;
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
        saw_trained = saw_trained || (state == "trained" && count == 2);
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
    if (!saw_excluded || !saw_trained || saw_pending) return 13;

    auto malformed_path = path;
    malformed_path += L".malformed.sqlite3";
    DeleteFileW(malformed_path.c_str());
    {
        TrainingDataWriter writer(malformed_path);
    }
    sqlite3* malformed_database = nullptr;
    if (sqlite3_open16(malformed_path.c_str(), &malformed_database) != SQLITE_OK) return 24;
    const char* malformed_insert =
        "INSERT INTO training_commits(event_id,context,answer,padding_json,reading,"
        "revice,committed_at_utc) VALUES "
        "('malformed:1','','','{','',0,'t'),"
        "('nonobject:1','','','[42]','',0,'t')";
    const int malformed_result = sqlite3_exec(
        malformed_database, malformed_insert, nullptr, nullptr, nullptr);
    sqlite3_close(malformed_database);
    if (malformed_result != SQLITE_OK) return 25;
    {
        TrainingDataWriter writer(malformed_path);
        if (writer.pending_count() != 2 || writer.pending_items().size() != 2) return 26;
    }
    DeleteFileW(malformed_path.c_str());
    auto malformed_wal_path = malformed_path;
    malformed_wal_path += L"-wal";
    DeleteFileW(malformed_wal_path.c_str());
    auto malformed_shm_path = malformed_path;
    malformed_shm_path += L"-shm";
    DeleteFileW(malformed_shm_path.c_str());
    return 0;
}
