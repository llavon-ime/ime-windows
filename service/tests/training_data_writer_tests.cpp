#include "service/training_data_writer.hpp"
#include "service/lora_dataset_builder.hpp"
#include "service/commit_crypto.hpp"
#include "service/training_data_cleanup.hpp"

#include <windows.h>
#include <sqlite3.h>

#include <filesystem>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using llavon::service::RawCommitEvent;
using llavon::service::RawCommitInputEntry;
using llavon::service::TrainingDataWriter;
using llavon::service::write_lora_numeric_dataset;

int delete_pending_tests(const std::filesystem::path& path, RawCommitEvent event) {
    using namespace std::chrono_literals;
    {
        TrainingDataWriter writer(path, 10s);
        writer.configure_password("delete-test");
        event.session_id = "delete";
        event.sequence = 1;
        writer.enqueue(event);
        event.sequence = 2;
        writer.enqueue(event);
        for (int attempt = 0; attempt < 200 && writer.pending_count() != 1; ++attempt) {
            std::this_thread::sleep_for(5ms);
        }
        if (writer.pending_count() != 1 || !writer.delete_pending(u"delete:1")) return 81;
        if (writer.pending_count() != 0 || !writer.pending_items().empty()) return 82;
        if (writer.delete_pending(u"delete:1") || writer.delete_pending(u"missing")) return 83;
    }
    std::filesystem::remove(path);
    return 0;
}

int reset_tests(const std::filesystem::path& path, RawCommitEvent event) {
    using namespace std::chrono_literals;
    const auto model = path.wstring() + L".gguf";
    { std::ofstream file(model, std::ios::binary); file << "preserved-model"; }
    {
        TrainingDataWriter writer(path, 10s);
        writer.configure_password("forgotten-password");
        event.session_id = "reset";
        event.sequence = 1;
        writer.enqueue(event);
        event.sequence = 2;
        writer.enqueue(event); // Flush the first record, stage the second.
        for (int attempt = 0; attempt < 200 && writer.pending_count() != 1; ++attempt) {
            std::this_thread::sleep_for(5ms);
        }
        if (writer.pending_count() != 1) return 70;
        const llavon::service::LoraTrainingRun run{
            .base_model_revision = "reset-test",
            .adapter_path = model,
            .output_model_path = model,
            .completed_at_utc = "2026-09-24T00:00:00Z",
            .record_count = 1,
            .cumulative_record_count = 1,
            .optimizer_steps = 1,
            .rank = 8,
            .alpha = 16,
            .target_modules = u"q_proj,v_proj",
        };
        if (!writer.complete_lora_training(run, {u"reset:1"})) return 71;
        event.sequence = 3;
        writer.enqueue(event);
        for (int attempt = 0; attempt < 200 && writer.pending_count() != 1; ++attempt) {
            std::this_thread::sleep_for(5ms);
        }
        if (!writer.exclude_unselected({}, {u"reset:2"})) return 72;
        // This operation requires no password and must also discard staged input.
        writer.reset_conversation_data();
        if (writer.pending_count() != 0 || !writer.pending_items().empty() ||
            writer.protection_status().configured || writer.protection_status().enabled ||
            writer.lora_training_history().size() != 1) return 73;
        sqlite3* database = nullptr;
        if (sqlite3_open16(path.c_str(), &database) != SQLITE_OK) return 74;
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM training_commits", -1,
                              &statement, nullptr) != SQLITE_OK) return 75;
        const bool empty = sqlite3_step(statement) == SQLITE_ROW && sqlite3_column_int(statement, 0) == 0;
        sqlite3_finalize(statement);
        sqlite3_close(database);
        if (!empty) return 76;
        writer.configure_password("new-password");
        event.sequence = 4;
        writer.enqueue(event);
        event.sequence = 5;
        writer.enqueue(event);
        for (int attempt = 0; attempt < 200 && writer.pending_count() != 1; ++attempt) {
            std::this_thread::sleep_for(5ms);
        }
        const auto items = writer.pending_items("new-password");
        if (items.size() != 1 || items.front().event_id != u"reset:4") return 77;
        try { (void)writer.pending_items("forgotten-password"); return 78; } catch (const std::exception&) {}
    }
    {
        TrainingDataWriter writer(path);
        const auto items = writer.pending_items("new-password");
        if (items.size() != 2 || items.back().event_id != u"reset:5" ||
            writer.lora_training_history().size() != 1) return 79;
    }
    {
        std::ifstream file(model, std::ios::binary);
        std::string text;
        file >> text;
        if (text != "preserved-model") return 80;
    }
    std::filesystem::remove(model);
    std::filesystem::remove(path);
    return 0;
}

int correction_staging_tests(
    const std::filesystem::path& path, const RawCommitEvent& prototype) {
    using namespace std::chrono_literals;
    std::filesystem::remove(path);

    auto first = prototype;
    first.session_id = "correction";
    first.sequence = 1;
    first.answer = u"wrong-one";
    auto second = prototype;
    second.session_id = first.session_id;
    second.sequence = 2;
    second.answer = u"wrong-two";

    // A discard is scoped to exactly the latest sequence in its session.
    {
        TrainingDataWriter writer(path, 200ms);
        writer.configure_password("staging-password");
        writer.enqueue(first);
        writer.enqueue(second);
        writer.discard_staged(second.session_id, second.sequence);
    }
    {
        TrainingDataWriter writer(path, 200ms);
        const auto items = writer.pending_items("staging-password");
        if (items.size() != 1 || items.front().event_id != u"correction:1" ||
            items.front().answer != first.answer) {
            return 62;
        }
    }

    // Once the staging deadline has passed, a late Backspace cannot remove it.
    auto expired = prototype;
    expired.session_id = "expired";
    expired.sequence = 1;
    expired.answer = u"stable";
    {
        TrainingDataWriter writer(path, 30ms);
        writer.enqueue(expired);
        std::this_thread::sleep_for(80ms);
        writer.discard_staged(expired.session_id, expired.sequence);
    }
    {
        TrainingDataWriter writer(path, 200ms);
        const auto items = writer.pending_items("staging-password");
        if (items.size() != 2 || items.back().event_id != u"expired:1") {
            return 63;
        }
    }

    std::filesystem::remove(path);
    return 0;
}

int protection_tests(const std::filesystem::path& path, RawCommitEvent event) {
    using namespace llavon::service;
    {
        TrainingDataWriter writer(path);
        writer.enqueue(event);
    }
    {
        TrainingDataWriter writer(path);
        if (writer.pending_count() != 0) return 40;
        writer.configure_password("test-password");
        writer.set_recording_enabled(false);
        writer.enqueue(event);
    }
    {
        TrainingDataWriter writer(path);
        if (writer.pending_count() != 0 || writer.protection_status().enabled) return 41;
        writer.set_recording_enabled(true);
        writer.enqueue(event);
    }
    {
        TrainingDataWriter writer(path);
        if (writer.pending_count() != 1 || !writer.protection_status().enabled) return 42;
        if (writer.pending_items("test-password").front().answer != event.answer) return 43;
        try { writer.configure_password("replacement"); return 44; } catch (const std::exception&) {}
    }
    sqlite3* database = nullptr;
    if (sqlite3_open16(path.c_str(), &database) != SQLITE_OK) return 45;
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database,
        "SELECT schema_version,context,answer,padding_json,reading FROM training_commits", -1,
        &statement, nullptr) != SQLITE_OK) return 46;
    if (sqlite3_step(statement) != SQLITE_ROW || sqlite3_column_int(statement, 0) != 2) return 47;
    for (int column = 1; column <= 4; ++column) {
        const std::string ciphertext(reinterpret_cast<const char*>(sqlite3_column_text(statement, column)));
        if (ciphertext.size() < 96 || ciphertext.find("sample") != std::string::npos ||
            ciphertext.find("result") != std::string::npos) return 48;
    }
    sqlite3_finalize(statement);
    // Swapping authenticated fields must fail even with the correct password.
    if (sqlite3_exec(database, "UPDATE training_commits SET answer=context", nullptr, nullptr, nullptr) != SQLITE_OK) return 49;
    sqlite3_close(database);
    {
        TrainingDataWriter writer(path);
        try { (void)writer.pending_items("test-password"); return 50; } catch (const std::exception&) {}
        try { (void)writer.pending_records({u"session:7"}, "test-password"); return 51; } catch (const std::exception&) {}
    }
    std::filesystem::remove(path);

    // Migrate a legacy plaintext row without ever making it readable unauthenticated.
    { TrainingDataWriter writer(path); }
    if (sqlite3_open16(path.c_str(), &database) != SQLITE_OK) return 52;
    const char* legacy = "INSERT INTO training_commits(event_id,context,answer,padding_json,reading,revice,committed_at_utc) "
        "VALUES('legacy:1','old-context','old-answer','[{\"syllable\":\"ㄅ\",\"tone\":1}]','ㄅ',0,'t')";
    if (sqlite3_exec(database, legacy, nullptr, nullptr, nullptr) != SQLITE_OK) return 53;
    sqlite3_close(database);
    {
        TrainingDataWriter writer(path);
        if (!writer.pending_items().front().answer.empty()) return 54;
        writer.configure_password("migration-password");
        const auto records = writer.pending_records({u"legacy:1"}, "migration-password");
        if (records.size() != 1 || records.front().answer != u"old-answer") return 55;
    }
    {
        std::ifstream file(path, std::ios::binary);
        const std::string bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        if (bytes.find("old-answer") != std::string::npos || bytes.find("old-context") != std::string::npos) return 56;
    }
    std::filesystem::remove(path);

    const auto cache = path.parent_path() / (path.filename().wstring() + L"-runs");
    const auto run = cache / L"one";
    std::filesystem::create_directories(run);
    { std::ofstream file(run / L"training.jsonl"); file << "plaintext"; }
    { std::ofstream file(run / L"training.jsonl.partial"); file << "plaintext"; }
    { std::ofstream file(run / L"adapter.safetensors"); file << "model"; }
    if (discard_plaintext_training_datasets(cache) != 2 ||
        !std::filesystem::exists(run / L"adapter.safetensors") ||
        discard_plaintext_training_datasets(cache) != 0) return 57;
    const auto locked = run / L"training.jsonl";
    HANDLE handle = CreateFileW(locked.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return 58;
    bool blocked = false;
    try { (void)discard_plaintext_training_datasets(cache); } catch (const std::exception&) { blocked = true; }
    CloseHandle(handle);
    if (!blocked || discard_plaintext_training_datasets(cache) != 1) return 59;
    std::filesystem::remove(run / L"adapter.safetensors");
    std::filesystem::remove(run);
    std::filesystem::remove(cache);

    commit_crypto::PublicParameters parameters;
    randombytes_buf(parameters.salt.data(), parameters.salt.size());
    commit_crypto::PrivateKey private_key;
    commit_crypto::derive("benchmark-password", parameters, private_key, false);
    const auto first = commit_crypto::seal("message", "id/answer", parameters);
    if (first == commit_crypto::seal("message", "id/answer", parameters) ||
        commit_crypto::open(first, "id/answer", parameters, private_key) != "message") return 60;
    auto damaged = first;
    damaged.back() = damaged.back() == '0' ? '1' : '0';
    try { (void)commit_crypto::open(damaged, "id/answer", parameters, private_key); return 61; } catch (const std::exception&) {}
    const std::string payload(1024, 'x');
    const auto begin = std::chrono::steady_clock::now();
    for (int record = 0; record < 100; ++record) {
        for (int field = 0; field < 4; ++field) (void)commit_crypto::seal(payload, "benchmark/field", parameters);
    }
    const auto microseconds = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count();
    std::cout << "Background encryption (4 x 1 KiB fields): " << microseconds / 100 << " us/commit\n";
    return 0;
}

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
        if (writer.protection_status().configured || writer.protection_status().enabled) return 30;
        writer.enqueue(event); // Default-off must not persist this event.
        writer.configure_password("0000");
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
        if (writer.pending_items()[0].answer != u"") return 31;
        try { (void)writer.pending_items("incorrect"); return 32; } catch (const std::exception&) {}
        try { (void)writer.pending_records({u"session:7"}); return 33; } catch (const std::exception&) {}
        const auto pending = writer.pending_items("0000");
        if (pending.size() != 4 || pending[0].event_id != u"session:7" ||
            pending[0].context != u"sample" || pending[0].answer != u"result" ||
            pending[0].reading.empty() || !pending[0].revice) {
            return 4;
        }
        const auto stored = writer.pending_records({u"session:7", u"s:1"}, "0000");
        if (stored.size() != 2 || !stored[0].revice || stored[1].revice ||
            stored[0].padding_json.find(R"("tone":3)") == std::string::npos ||
            stored[0].padding_json.find(R"("tone":1)") == std::string::npos ||
            stored[0].padding_json.find(R"({"literal":",")") == std::string::npos ||
            stored[1].padding_json.find(R"({"rawReading":)") == std::string::npos) {
            return 5;
        }
        const auto records = writer.pending_records({u"train:1"}, "0000");
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
    auto protection_path = path;
    protection_path += L".protection.sqlite3";
    auto reset_path = path;
    reset_path += L".reset.sqlite3";
    auto delete_path = path;
    delete_path += L".delete.sqlite3";
    if (const int result = delete_pending_tests(delete_path, event); result != 0) return result;
    if (const int result = reset_tests(reset_path, event); result != 0) return result;
    auto correction_path = path;
    correction_path += L".correction.sqlite3";
    if (const int result = correction_staging_tests(correction_path, event);
        result != 0) {
        return result;
    }
    return protection_tests(protection_path, event);
}
