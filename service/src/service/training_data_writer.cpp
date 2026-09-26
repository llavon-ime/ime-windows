#include "training_data_writer.hpp"
#include "commit_crypto.hpp"

#include <shlobj.h>
#include <sqlite3.h>
#include <rfl/Generic.hpp>
#include <rfl/json.hpp>
#include <utf8/cpp20.h>
#include <windows.h>

#include <algorithm>
#include <fstream>
#include <iostream>
#include <list>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

namespace llavon::service {
namespace {

constexpr wchar_t database_path_environment[] =
    L"LLAVON_IME_TRAINING_DATABASE_PATH";
constexpr wchar_t default_directory_name[] = L"Llavon IME";
constexpr wchar_t default_data_directory_name[] = L"training-data";
constexpr wchar_t default_filename[] = L"commits.sqlite3";

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;

    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return std::filesystem::path(std::move(value));
}

std::filesystem::path default_database_path() {
    if (auto configured = environment_path(database_path_environment)) {
        return *configured;
    }
    PWSTR local_app_data = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data);
    if (FAILED(result)) {
        throw std::runtime_error("unable to resolve LocalAppData for training data");
    }
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(
        local_app_data, CoTaskMemFree);
    return std::filesystem::path(owned_path.get()) / default_directory_name /
           default_data_directory_name / default_filename;
}

[[noreturn]] void throw_sqlite(sqlite3* database, std::string_view operation) {
    std::string message(operation);
    message += ": ";
    message += database ? sqlite3_errmsg(database) : "unable to open database";
    throw std::runtime_error(message);
}

class Database final {
public:
    explicit Database(const std::filesystem::path& path) {
        const std::string path_utf8 = utf8::utf16to8(path.u16string());
        if (sqlite3_open_v2(path_utf8.c_str(), &handle_,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr) != SQLITE_OK) {
            const std::string message = handle_ ? sqlite3_errmsg(handle_)
                                                : "unable to open database";
            if (handle_) sqlite3_close(handle_);
            handle_ = nullptr;
            throw std::runtime_error(message);
        }
        sqlite3_busy_timeout(handle_, 3000);
        execute("PRAGMA foreign_keys=ON");
        execute("PRAGMA synchronous=NORMAL");
    }

    ~Database() {
        if (handle_) sqlite3_close(handle_);
    }

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    sqlite3* get() const noexcept { return handle_; }

    void execute(const char* sql) const {
        char* error = nullptr;
        if (sqlite3_exec(handle_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : sqlite3_errmsg(handle_);
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }

private:
    sqlite3* handle_ = nullptr;
};

class Statement final {
public:
    Statement(sqlite3* database, const char* sql) : database_(database) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            throw_sqlite(database, "prepare statement");
        }
    }

    ~Statement() {
        if (statement_) sqlite3_finalize(statement_);
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    sqlite3_stmt* get() const noexcept { return statement_; }

    void bind_text(int index, std::string_view value) {
        if (sqlite3_bind_text(statement_, index, value.data(),
                              static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite(database_, "bind text");
        }
    }

    void bind_integer(int index, sqlite3_int64 value) {
        if (sqlite3_bind_int64(statement_, index, value) != SQLITE_OK) {
            throw_sqlite(database_, "bind integer");
        }
    }

    void bind_real(int index, double value) {
        if (sqlite3_bind_double(statement_, index, value) != SQLITE_OK) {
            throw_sqlite(database_, "bind real");
        }
    }

    void bind_null(int index) {
        if (sqlite3_bind_null(statement_, index) != SQLITE_OK) {
            throw_sqlite(database_, "bind null");
        }
    }

    void execute() {
        if (sqlite3_step(statement_) != SQLITE_DONE) {
            throw_sqlite(database_, "execute statement");
        }
    }

    bool next() {
        const int result = sqlite3_step(statement_);
        if (result == SQLITE_ROW) return true;
        if (result == SQLITE_DONE) return false;
        throw_sqlite(database_, "read row");
    }

    void reset() {
        if (sqlite3_reset(statement_) != SQLITE_OK ||
            sqlite3_clear_bindings(statement_) != SQLITE_OK) {
            throw_sqlite(database_, "reset statement");
        }
    }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void append_json_string(std::string& output, std::string_view value) {
    output.push_back('"');
    constexpr char hex[] = "0123456789abcdef";
    for (const unsigned char ch : value) {
        switch (ch) {
            case '"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20) {
                    output += "\\u00";
                    output.push_back(hex[ch >> 4]);
                    output.push_back(hex[ch & 0x0f]);
                } else {
                    output.push_back(static_cast<char>(ch));
                }
                break;
        }
    }
    output.push_back('"');
}

std::uint8_t remove_tone(std::u16string& reading) {
    if (reading.empty()) return 0;
    std::uint8_t tone = 0;
    switch (reading.back()) {
        case u' ': tone = 1; break;
        case u'ˊ': tone = 2; break;
        case u'ˇ': tone = 3; break;
        case u'ˋ': tone = 4; break;
        case u'˙': tone = 5; break;
        default: return 0;
    }
    reading.pop_back();
    return reading.empty() ? 0 : tone;
}

std::string serialize_padding(const RawCommitEvent& event) {
    std::string output = "[";
    bool first = true;
    for (const auto& entry : event.input) {
        if (!first) output.push_back(',');
        first = false;
        std::u16string syllable = entry.reading;
        const std::uint8_t tone = remove_tone(syllable);
        if (tone != 0) {
            output += R"({"syllable":)";
            append_json_string(output, utf8::utf16to8(syllable));
            output += R"(,"tone":)" + std::to_string(tone) + "}";
        } else if (entry.reading.empty() && !entry.output.empty()) {
            output += R"({"literal":)";
            append_json_string(output, utf8::utf16to8(entry.output));
            output.push_back('}');
        } else {
            output += R"({"rawReading":)";
            append_json_string(output, utf8::utf16to8(entry.reading));
            output.push_back('}');
        }
    }
    output.push_back(']');
    return output;
}

std::u16string display_reading(const RawCommitEvent& event) {
    std::u16string output;
    bool first = true;
    for (const auto& entry : event.input) {
        if (!first) output += u" ";
        first = false;
        output += !entry.reading.empty() ? entry.reading : entry.output;
    }
    return output;
}

bool was_revised(const RawCommitEvent& event) {
    for (const auto& entry : event.input) {
        if (entry.manually_selected) return true;
    }
    return false;
}

bool has_bopomofo(std::u16string_view reading) {
    return std::any_of(reading.begin(), reading.end(), [](char16_t character) {
        return character >= u'ㄅ' && character <= u'ㄩ';
    });
}

bool has_bopomofo_input(const RawCommitEvent& event) {
    return std::any_of(event.input.begin(), event.input.end(), [](const auto& entry) {
        return has_bopomofo(entry.reading);
    });
}

std::string column_text(sqlite3_stmt* statement, int column);

bool has_bopomofo_annotation(std::string_view padding_json) {
    try {
        const auto padding = rfl::json::read<rfl::Generic>(padding_json);
        if (!padding) return true; // Leave malformed data untouched.
        const auto* entries = std::get_if<rfl::Generic::Array>(&padding->get());
        if (!entries) return true;
        for (const auto& entry : *entries) {
            const auto* object = std::get_if<rfl::Generic::Object>(&entry.get());
            if (!object) return true;
            for (const char* key : {"syllable", "rawReading"}) {
                if (!object->count(key)) continue;
                const auto value = object->at(key).to_string();
                if (value && has_bopomofo(utf8::utf8to16(*value))) {
                    return true;
                }
            }
        }
        return false;
    } catch (...) {
        return true; // Do not delete data whose format we cannot inspect.
    }
}

void discard_existing_non_bopomofo_commits(Database& database) {
    sqlite3_int64 last_id = 0;
    while (true) {
        std::vector<sqlite3_int64> ids;
        sqlite3_int64 next_id = last_id;
        {
            Statement select(database.get(),
                "SELECT id, padding_json FROM training_commits "
                "WHERE training_state='pending' AND schema_version=1 AND id>? "
                "AND NOT EXISTS (SELECT 1 FROM legacy_training_unknown "
                "WHERE commit_id=training_commits.id) "
                "ORDER BY id LIMIT 512");
            select.bind_integer(1, last_id);
            while (select.next()) {
                next_id = sqlite3_column_int64(select.get(), 0);
                if (!has_bopomofo_annotation(column_text(select.get(), 1))) {
                    ids.push_back(next_id);
                }
            }
        }
        if (next_id == last_id) return;
        last_id = next_id;
        if (ids.empty()) continue;
        database.execute("BEGIN IMMEDIATE");
        try {
            Statement remove(database.get(),
                "DELETE FROM training_commits WHERE id=? AND training_state='pending'");
            for (const auto id : ids) {
                remove.bind_integer(1, id);
                remove.execute();
                remove.reset();
            }
            database.execute("COMMIT");
        } catch (...) {
            try { database.execute("ROLLBACK"); } catch (...) {}
            throw;
        }
    }
}

void initialize_database(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    Database database(path);
    database.execute("PRAGMA journal_mode=WAL");
    database.execute(
        "CREATE TABLE IF NOT EXISTS commit_protection ("
        "id INTEGER PRIMARY KEY CHECK(id=1), version INTEGER NOT NULL CHECK(version=1),"
        "salt TEXT NOT NULL, public_key TEXT NOT NULL, enabled INTEGER NOT NULL CHECK(enabled IN (0,1)))");
    database.execute(
        "CREATE TABLE IF NOT EXISTS training_commits ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "event_id TEXT NOT NULL UNIQUE,"
        "schema_version INTEGER NOT NULL DEFAULT 1,"
        "context TEXT NOT NULL,"
        "answer TEXT NOT NULL,"
        "padding_json TEXT NOT NULL,"
        "reading TEXT NOT NULL,"
        "revice INTEGER NOT NULL CHECK (revice IN (0, 1)),"
        "event_type TEXT NOT NULL DEFAULT 'commit',"
        "committed_at_utc TEXT NOT NULL,"
        "revision_of TEXT NULL,"
        "training_state TEXT NOT NULL DEFAULT 'pending' "
        "CHECK (training_state IN ('pending', 'excluded', 'trained'))"
        ")");
    database.execute(
        "CREATE INDEX IF NOT EXISTS training_commits_state_id "
        "ON training_commits(training_state, id)");
    database.execute(
        "CREATE TABLE IF NOT EXISTS lora_training_runs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "parent_id INTEGER NULL REFERENCES lora_training_runs(id),"
        "base_model_revision TEXT NOT NULL,"
        "adapter_path TEXT NOT NULL,"
        "output_model_path TEXT NOT NULL,"
        "completed_at_utc TEXT NOT NULL,"
        "record_count INTEGER NOT NULL CHECK(record_count > 0),"
        "cumulative_record_count INTEGER NOT NULL CHECK(cumulative_record_count > 0),"
        "optimizer_steps INTEGER NOT NULL CHECK(optimizer_steps >= 0),"
        "rank INTEGER NOT NULL CHECK(rank > 0),"
        "alpha REAL NOT NULL CHECK(alpha > 0),"
        "dropout REAL NOT NULL CHECK(dropout >= 0 AND dropout < 1),"
        "target_modules TEXT NOT NULL,"
        "training_request_json TEXT NULL CHECK(training_request_json IS NULL OR json_valid(training_request_json))"
        ")");
    database.execute(
        "CREATE INDEX IF NOT EXISTS lora_training_runs_completed_id "
        "ON lora_training_runs(completed_at_utc, id)");
    database.execute("BEGIN IMMEDIATE");
    try {
        bool has_request = false;
        Statement columns(database.get(), "PRAGMA table_info(lora_training_runs)");
        while (columns.next()) {
            if (column_text(columns.get(), 1) == "training_request_json") {
                has_request = true;
            }
        }
        if (!has_request) {
            database.execute(
                "ALTER TABLE lora_training_runs ADD COLUMN training_request_json TEXT NULL "
                "CHECK(training_request_json IS NULL OR json_valid(training_request_json))");
        }
        database.execute(
            "CREATE INDEX IF NOT EXISTS lora_training_runs_parent_id "
            "ON lora_training_runs(parent_id)");
        database.execute(
            "CREATE TABLE IF NOT EXISTS lora_run_commits ("
            "run_id INTEGER NOT NULL REFERENCES lora_training_runs(id),"
            "commit_id INTEGER NOT NULL REFERENCES training_commits(id) ON DELETE CASCADE,"
            "PRIMARY KEY(run_id, commit_id)) WITHOUT ROWID");
        database.execute(
            "CREATE INDEX IF NOT EXISTS lora_run_commits_commit_run "
            "ON lora_run_commits(commit_id, run_id)");
        database.execute(
            "CREATE TABLE IF NOT EXISTS legacy_training_unknown ("
            "commit_id INTEGER PRIMARY KEY REFERENCES training_commits(id) ON DELETE CASCADE)");
        database.execute(
            "INSERT OR IGNORE INTO legacy_training_unknown(commit_id) "
            "SELECT id FROM training_commits WHERE training_state='trained'");
        database.execute(
            "UPDATE training_commits SET training_state='pending' "
            "WHERE training_state='trained'");
        std::vector<std::pair<sqlite3_int64, std::filesystem::path>> prior_requests;
        {
            Statement runs(database.get(),
                "SELECT id, adapter_path FROM lora_training_runs "
                "WHERE training_request_json IS NULL");
            while (runs.next()) {
                const auto adapter = std::filesystem::path(
                    utf8::utf8to16(column_text(runs.get(), 1)));
                prior_requests.emplace_back(sqlite3_column_int64(runs.get(), 0),
                    adapter.parent_path() / L"training_request.json");
            }
        }
        Statement valid_json(database.get(), "SELECT json_valid(?)");
        Statement save_request(database.get(),
            "UPDATE lora_training_runs SET "
            "training_request_json=json_remove(?,'$.strength') WHERE id=?");
        for (const auto& [id, request_path] : prior_requests) {
            std::error_code error;
            if (!std::filesystem::is_regular_file(request_path, error) ||
                std::filesystem::file_size(request_path, error) > 256 * 1024 || error) {
                continue;
            }
            std::ifstream input(request_path, std::ios::binary);
            const std::string request{std::istreambuf_iterator<char>(input),
                                      std::istreambuf_iterator<char>()};
            if (request.empty()) continue;
            valid_json.bind_text(1, request);
            const bool valid = valid_json.next() &&
                sqlite3_column_int(valid_json.get(), 0) == 1;
            valid_json.reset();
            if (!valid) continue;
            save_request.bind_text(1, request);
            save_request.bind_integer(2, id);
            save_request.execute();
            save_request.reset();
        }
        const auto database_version = [&] {
            Statement version(database.get(), "PRAGMA user_version");
            if (!version.next()) {
                throw std::runtime_error("unable to read training database version");
            }
            return sqlite3_column_int(version.get(), 0);
        }();
        if (database_version < 2) {
            database.execute("PRAGMA user_version=2");
        }
        database.execute("COMMIT");
    } catch (...) {
        try { database.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (!value) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

constexpr std::string_view eligible_commits_cte =
    "WITH RECURSIVE lineage(id, parent_id) AS ("
    "SELECT id, parent_id FROM lora_training_runs WHERE id=?1 "
    "UNION ALL SELECT parent.id, parent.parent_id FROM lora_training_runs parent "
    "JOIN lineage child ON parent.id=child.parent_id) ";

constexpr std::string_view eligible_commits_where =
    "training_state='pending' "
    "AND NOT EXISTS (SELECT 1 FROM legacy_training_unknown legacy "
    "WHERE legacy.commit_id=training_commits.id) "
    "AND NOT EXISTS (SELECT 1 FROM lora_run_commits used "
    "JOIN lineage ON lineage.id=used.run_id "
    "WHERE used.commit_id=training_commits.id) ";

void validate_base_run(Database& database, std::int64_t base_run_id) {
    if (base_run_id == 0) return;
    if (base_run_id < 0) throw std::invalid_argument("invalid LoRA base run ID");
    Statement select(database.get(), "SELECT 1 FROM lora_training_runs WHERE id=?");
    select.bind_integer(1, base_run_id);
    if (!select.next()) throw std::invalid_argument("LoRA base run does not exist");
}

}  // namespace

TrainingDataWriter::TrainingDataWriter(
    std::optional<std::filesystem::path> database_path,
    std::chrono::steady_clock::duration staging_window)
    : database_path_(database_path ? std::move(*database_path)
                                   : default_database_path()),
      staging_window_(staging_window) {
    if (staging_window_ <= std::chrono::steady_clock::duration::zero()) {
        throw std::invalid_argument("staging window must be positive");
    }
    initialize_database(database_path_);
    commit_crypto::initialize();
    recording_enabled_.store(protection_status().enabled);
    {
        Database database(database_path_);
        const auto database_version = [&] {
            Statement version(database.get(), "PRAGMA user_version");
            if (!version.next()) {
                throw std::runtime_error("unable to read training database version");
            }
            return sqlite3_column_int(version.get(), 0);
        }();
        if (database_version < 3) {
            discard_existing_non_bopomofo_commits(database);
            database.execute("PRAGMA user_version=3");
        }
        if (database_version < 4) {
            database.execute(
                "UPDATE lora_training_runs SET "
                "training_request_json=json_remove(training_request_json,'$.strength') "
                "WHERE training_request_json IS NOT NULL "
                "AND json_type(training_request_json,'$.strength') IS NOT NULL");
            database.execute("PRAGMA user_version=4");
        }
        const auto latest = latest_lora_training_run();
        pending_count_.store(available_count(latest ? latest->id : 0),
                             std::memory_order_release);
    }
    worker_ = std::thread([this] { worker_main(); });
}

TrainingDataWriter::~TrainingDataWriter() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    available_.notify_one();
    if (worker_.joinable()) worker_.join();
}

void TrainingDataWriter::enqueue(RawCommitEvent event) {
    if (!recording_enabled_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || !recording_enabled_.load(std::memory_order_acquire) || queue_.size() >= 256) return;
        queue_.push_back(QueuedOperation{
            .kind = QueuedOperation::Kind::commit,
            .event = std::move(event),
            .queued_at = std::chrono::steady_clock::now(),
            .generation = generation_.load(std::memory_order_acquire),
        });
    }
    available_.notify_one();
}

void TrainingDataWriter::discard_staged(
    std::string session_id, std::uint64_t sequence) {
    if (!recording_enabled_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard lock(mutex_);
        if (stopping_ || !recording_enabled_.load(std::memory_order_acquire)) return;
        queue_.push_back(QueuedOperation{
            .kind = QueuedOperation::Kind::discard,
            .session_id = std::move(session_id),
            .sequence = sequence,
            .queued_at = std::chrono::steady_clock::now(),
        });
    }
    available_.notify_one();
}

void TrainingDataWriter::set_pending_count_callback(
    std::function<void(std::size_t)> callback) {
    std::lock_guard lock(callback_mutex_);
    pending_count_callback_ = std::move(callback);
    try {
        if (pending_count_callback_) {
            pending_count_callback_(pending_count_.load(std::memory_order_acquire));
        }
    } catch (...) {
        // Settings UI notifications are optional; database writes continue.
    }
}

void TrainingDataWriter::publish_pending_delta(std::ptrdiff_t delta) noexcept {
    if (delta == 0) return;
    std::lock_guard lock(callback_mutex_);
    const std::size_t previous = pending_count_.load(std::memory_order_relaxed);
    const std::size_t current = delta > 0
        ? previous + static_cast<std::size_t>(delta)
        : previous - std::min(previous, static_cast<std::size_t>(-delta));
    pending_count_.store(current, std::memory_order_release);
    try {
        if (pending_count_callback_) pending_count_callback_(current);
    } catch (...) {
        // A settings UI failure must not stop commit persistence.
    }
}

void TrainingDataWriter::publish_latest_available_count() noexcept {
    try {
        const auto latest = latest_lora_training_run();
        const auto count = available_count(latest ? latest->id : 0);
        std::lock_guard lock(callback_mutex_);
        pending_count_.store(count, std::memory_order_release);
        if (pending_count_callback_) pending_count_callback_(count);
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to count available LoRA records: "
                  << error.what() << '\n';
    } catch (...) {
    }
}

namespace {
commit_crypto::PublicParameters read_parameters(Database& database) {
    Statement select(database.get(), "SELECT version,salt,public_key FROM commit_protection WHERE id=1");
    if (!select.next() || sqlite3_column_int(select.get(), 0) != 1) {
        throw std::runtime_error("commit password is not configured");
    }
    commit_crypto::PublicParameters parameters;
    commit_crypto::unhex(column_text(select.get(), 1), parameters.salt);
    commit_crypto::unhex(column_text(select.get(), 2), parameters.key);
    return parameters;
}
}

TrainingDataWriter::ProtectionStatus TrainingDataWriter::protection_status() const {
    Database database(database_path_);
    Statement select(database.get(), "SELECT enabled FROM commit_protection WHERE id=1");
    if (!select.next()) return {false, false};
    return {true, sqlite3_column_int(select.get(), 0) != 0};
}

void TrainingDataWriter::set_recording_enabled(bool enabled) {
    std::lock_guard protection_lock(protection_mutex_);
    Database database(database_path_);
    if (enabled) (void)read_parameters(database);
    Statement update(database.get(), "UPDATE commit_protection SET enabled=? WHERE id=1");
    update.bind_integer(1, enabled ? 1 : 0);
    update.execute();
    recording_enabled_.store(enabled, std::memory_order_release);
    if (!enabled) {
        std::lock_guard lock(mutex_);
        queue_.clear();
        queue_.push_back(QueuedOperation{
            .kind = QueuedOperation::Kind::clear,
            .queued_at = std::chrono::steady_clock::now(),
        });
    }
    available_.notify_one();
}

void TrainingDataWriter::reset_conversation_data() {
    std::lock_guard protection_lock(protection_mutex_);
    recording_enabled_.store(false, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        generation_.fetch_add(1, std::memory_order_acq_rel);
        queue_.clear();
        queue_.push_back(QueuedOperation{.kind = QueuedOperation::Kind::clear});
    }
    available_.notify_one();
    Database database(database_path_);
    database.execute("PRAGMA secure_delete=ON");
    database.execute("BEGIN IMMEDIATE");
    try {
        database.execute("DELETE FROM training_commits");
        database.execute("DELETE FROM commit_protection");
        database.execute("COMMIT");
    } catch (...) {
        try { database.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
    publish_pending_delta(-static_cast<std::ptrdiff_t>(pending_count()));
    database.execute("VACUUM");
    if (sqlite3_wal_checkpoint_v2(database.get(), nullptr, SQLITE_CHECKPOINT_TRUNCATE,
                                 nullptr, nullptr) != SQLITE_OK) {
        throw_sqlite(database.get(), "clear conversation journal");
    }
}

void TrainingDataWriter::configure_password(std::string_view password) {
    std::lock_guard protection_lock(protection_mutex_);
    if (protection_status().configured) throw std::runtime_error("password already configured");
    commit_crypto::PublicParameters parameters;
    randombytes_buf(parameters.salt.data(), parameters.salt.size());
    commit_crypto::PrivateKey private_key;
    commit_crypto::derive(password, parameters, private_key, false);
    Database database(database_path_);
    database.execute("PRAGMA secure_delete=ON");
    database.execute("BEGIN IMMEDIATE");
    try {
        Statement insert(database.get(),
            "INSERT INTO commit_protection VALUES(1,1,?,?,0)");
        insert.bind_text(1, commit_crypto::hex(parameters.salt));
        insert.bind_text(2, commit_crypto::hex(parameters.key));
        insert.execute();
        Statement select(database.get(),
            "SELECT event_id,context,answer,padding_json,reading FROM training_commits WHERE schema_version=1");
        Statement update(database.get(),
            "UPDATE training_commits SET schema_version=2,context=?,answer=?,padding_json=?,reading=? WHERE event_id=?");
        constexpr const char* fields[]{"context", "answer", "padding_json", "reading"};
        while (select.next()) {
            const auto id = column_text(select.get(), 0);
            for (int index = 0; index < 4; ++index) {
                auto plaintext = column_text(select.get(), index + 1);
                const auto ciphertext = commit_crypto::seal(plaintext, id + "/" + fields[index], parameters);
                sodium_memzero(plaintext.data(), plaintext.size());
                update.bind_text(index + 1, ciphertext);
            }
            update.bind_text(5, id);
            update.execute();
            update.reset();
        }
        database.execute("COMMIT");
    } catch (...) {
        try { database.execute("ROLLBACK"); } catch (...) {}
        throw;
    }
    database.execute("VACUUM");
    database.execute("PRAGMA wal_checkpoint(TRUNCATE)");
    database.execute("UPDATE commit_protection SET enabled=1 WHERE id=1");
    recording_enabled_.store(true, std::memory_order_release);
}

std::vector<TrainingDataItem> TrainingDataWriter::pending_items(
    std::string_view password, std::int64_t base_run_id) const {
    Database database(database_path_);
    validate_base_run(database, base_run_id);
    commit_crypto::PublicParameters parameters;
    commit_crypto::PrivateKey private_key;
    if (!password.empty()) {
        parameters = read_parameters(database);
        commit_crypto::derive(password, parameters, private_key, true);
    }
    const std::string query = std::string(eligible_commits_cte) +
        (password.empty()
            ? "SELECT event_id, revice FROM training_commits WHERE "
            : "SELECT event_id, context, answer, reading, revice, schema_version "
              "FROM training_commits WHERE ") +
        std::string(eligible_commits_where) + "ORDER BY id";
    Statement select(database.get(), query.c_str());
    select.bind_integer(1, base_run_id);
    std::vector<TrainingDataItem> result;
    while (select.next()) {
        const auto id = column_text(select.get(), 0);
        if (password.empty()) {
            result.push_back(TrainingDataItem{
                .event_id = utf8::utf8to16(id),
                .revice = sqlite3_column_int(select.get(), 1) != 0,
            });
            continue;
        }
        const auto decrypt = [&](int column, const char* field) {
            if (sqlite3_column_int(select.get(), 5) != 2) {
                throw std::runtime_error("unencrypted training record blocked");
            }
            return utf8::utf8to16(commit_crypto::open(column_text(select.get(), column),
                id + "/" + field, parameters, private_key));
        };
        result.push_back(TrainingDataItem{
            .event_id = utf8::utf8to16(id),
            .context = decrypt(1, "context"),
            .answer = decrypt(2, "answer"),
            .reading = decrypt(3, "reading"),
            .revice = sqlite3_column_int(select.get(), 4) != 0,
        });
    }
    return result;
}

std::vector<TrainingDataRecord> TrainingDataWriter::pending_records(
    const std::vector<std::u16string>& event_ids, std::string_view password,
    std::int64_t base_run_id) const {
    Database database(database_path_);
    validate_base_run(database, base_run_id);
    auto parameters = read_parameters(database);
    commit_crypto::PrivateKey private_key;
    commit_crypto::derive(password, parameters, private_key, true);
    const std::string query = std::string(eligible_commits_cte) +
        "SELECT event_id, context, answer, padding_json, revice, schema_version "
        "FROM training_commits WHERE " + std::string(eligible_commits_where) +
        "AND event_id=?2";
    Statement select(database.get(), query.c_str());
    select.bind_integer(1, base_run_id);
    std::vector<TrainingDataRecord> result;
    result.reserve(event_ids.size());
    for (const auto& event_id : event_ids) {
        select.bind_text(2, utf8::utf16to8(event_id));
        if (select.next()) {
            if (sqlite3_column_int(select.get(), 5) != 2) {
                throw std::runtime_error("unencrypted training record blocked");
            }
            const auto id = column_text(select.get(), 0);
            const auto decrypt = [&](int column, const char* field) {
                return commit_crypto::open(column_text(select.get(), column),
                    id + "/" + field, parameters, private_key);
            };
            result.push_back(TrainingDataRecord{
                .event_id = utf8::utf8to16(column_text(select.get(), 0)),
                .context = utf8::utf8to16(decrypt(1, "context")),
                .answer = utf8::utf8to16(decrypt(2, "answer")),
                .padding_json = decrypt(3, "padding_json"),
                .revice = sqlite3_column_int(select.get(), 4) != 0,
            });
        }
        select.reset();
        select.bind_integer(1, base_run_id);
    }
    return result;
}

std::size_t TrainingDataWriter::available_count(std::int64_t base_run_id) const {
    Database database(database_path_);
    validate_base_run(database, base_run_id);
    const std::string query = std::string(eligible_commits_cte) +
        "SELECT COUNT(*) FROM training_commits WHERE " +
        std::string(eligible_commits_where);
    Statement select(database.get(), query.c_str());
    select.bind_integer(1, base_run_id);
    if (!select.next()) throw std::runtime_error("unable to count LoRA training data");
    return static_cast<std::size_t>(sqlite3_column_int64(select.get(), 0));
}

bool TrainingDataWriter::delete_pending(std::u16string_view event_id) noexcept {
    try {
        Database database(database_path_);
        database.execute("PRAGMA secure_delete=ON");
        Statement remove(database.get(),
            "DELETE FROM training_commits WHERE event_id=? AND training_state='pending'");
        remove.bind_text(1, utf8::utf16to8(std::u16string(event_id)));
        remove.execute();
        if (sqlite3_changes(database.get()) != 1) return false;
        publish_latest_available_count();
        return true;
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to delete training data: " << error.what() << '\n';
        return false;
    }
}

bool TrainingDataWriter::exclude_unselected(
    const std::vector<std::u16string>& selected_event_ids,
    const std::vector<std::u16string>& reviewed_event_ids) noexcept {
    try {
        Database database(database_path_);
        database.execute("BEGIN IMMEDIATE");
        try {
            database.execute(
                "CREATE TEMP TABLE IF NOT EXISTS selected_training_events ("
                "event_id TEXT PRIMARY KEY)");
            database.execute(
                "CREATE TEMP TABLE IF NOT EXISTS reviewed_training_events ("
                "event_id TEXT PRIMARY KEY)");
            database.execute("DELETE FROM selected_training_events");
            database.execute("DELETE FROM reviewed_training_events");
            Statement insert_selected(
                database.get(),
                "INSERT OR IGNORE INTO selected_training_events(event_id) VALUES (?)");
            for (const auto& id : selected_event_ids) {
                insert_selected.bind_text(1, utf8::utf16to8(id));
                insert_selected.execute();
                insert_selected.reset();
            }
            Statement insert_reviewed(
                database.get(),
                "INSERT OR IGNORE INTO reviewed_training_events(event_id) VALUES (?)");
            for (const auto& id : reviewed_event_ids) {
                insert_reviewed.bind_text(1, utf8::utf16to8(id));
                insert_reviewed.execute();
                insert_reviewed.reset();
            }
            database.execute(
                "UPDATE training_commits SET training_state='excluded' "
                "WHERE training_state='pending' "
                "AND event_id IN (SELECT event_id FROM reviewed_training_events) "
                "AND event_id NOT IN (SELECT event_id FROM selected_training_events)");
            database.execute("COMMIT");
            publish_latest_available_count();
            return true;
        } catch (...) {
            try {
                database.execute("ROLLBACK");
            } catch (...) {
            }
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to exclude training data: " << error.what() << '\n';
        return false;
    }
}

namespace {

LoraTrainingRun read_lora_training_run(sqlite3_stmt* statement) {
    return LoraTrainingRun{
        .id = sqlite3_column_int64(statement, 0),
        .parent_id = sqlite3_column_type(statement, 1) == SQLITE_NULL
            ? 0 : sqlite3_column_int64(statement, 1),
        .base_model_revision = column_text(statement, 2),
        .adapter_path = std::filesystem::path(
            utf8::utf8to16(column_text(statement, 3))),
        .output_model_path = std::filesystem::path(
            utf8::utf8to16(column_text(statement, 4))),
        .completed_at_utc = column_text(statement, 5),
        .record_count = static_cast<std::size_t>(sqlite3_column_int64(statement, 6)),
        .cumulative_record_count = static_cast<std::size_t>(
            sqlite3_column_int64(statement, 7)),
        .optimizer_steps = sqlite3_column_int64(statement, 8),
        .rank = sqlite3_column_int(statement, 9),
        .alpha = sqlite3_column_double(statement, 10),
        .dropout = sqlite3_column_double(statement, 11),
        .target_modules = utf8::utf8to16(column_text(statement, 12)),
        .training_request_json = column_text(statement, 13),
    };
}

constexpr const char* select_lora_training_run =
    "SELECT id,parent_id,base_model_revision,adapter_path,output_model_path,"
    "completed_at_utc,record_count,cumulative_record_count,optimizer_steps,"
    "rank,alpha,dropout,target_modules,training_request_json "
    "FROM lora_training_runs ";

}  // namespace

std::optional<LoraTrainingRun>
TrainingDataWriter::latest_lora_training_run() const {
    Database database(database_path_);
    const std::string sql = std::string(select_lora_training_run) +
        "ORDER BY id DESC LIMIT 1";
    Statement select(database.get(), sql.c_str());
    if (!select.next()) return std::nullopt;
    return read_lora_training_run(select.get());
}

std::optional<LoraTrainingRun>
TrainingDataWriter::lora_training_run(std::int64_t id) const {
    if (id <= 0) return std::nullopt;
    Database database(database_path_);
    const std::string sql = std::string(select_lora_training_run) + "WHERE id=?";
    Statement select(database.get(), sql.c_str());
    select.bind_integer(1, id);
    if (!select.next()) return std::nullopt;
    return read_lora_training_run(select.get());
}

std::vector<LoraTrainingRun>
TrainingDataWriter::lora_training_history() const noexcept {
    try {
        Database database(database_path_);
        const std::string sql = std::string(select_lora_training_run) +
            "ORDER BY id";
        Statement select(database.get(), sql.c_str());
        std::vector<LoraTrainingRun> result;
        while (select.next()) result.push_back(read_lora_training_run(select.get()));
        return result;
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to read LoRA training history: "
                  << error.what() << '\n';
        return {};
    }
}

bool TrainingDataWriter::complete_lora_training(
    const LoraTrainingRun& run,
    const std::vector<std::u16string>& trained_event_ids) noexcept {
    try {
        Database database(database_path_);
        database.execute("BEGIN IMMEDIATE");
        try {
            std::size_t parent_count = 0;
            if (run.parent_id != 0) {
                Statement parent(database.get(),
                    "SELECT base_model_revision,cumulative_record_count "
                    "FROM lora_training_runs WHERE id=?");
                parent.bind_integer(1, run.parent_id);
                if (!parent.next() ||
                    column_text(parent.get(), 0) != run.base_model_revision) {
                    throw std::invalid_argument("invalid LoRA parent run");
                }
                parent_count = static_cast<std::size_t>(
                    sqlite3_column_int64(parent.get(), 1));
            }
            if (run.record_count == 0 ||
                run.cumulative_record_count != parent_count + run.record_count) {
                throw std::invalid_argument("invalid LoRA cumulative record count");
            }
            Statement insert(
                database.get(),
                "INSERT INTO lora_training_runs("
                "parent_id,base_model_revision,adapter_path,output_model_path,"
                "completed_at_utc,record_count,cumulative_record_count,optimizer_steps,"
                "rank,alpha,dropout,target_modules,training_request_json) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,json_remove(?,'$.strength'))");
            if (run.parent_id == 0) insert.bind_null(1);
            else insert.bind_integer(1, run.parent_id);
            insert.bind_text(2, run.base_model_revision);
            insert.bind_text(3, utf8::utf16to8(run.adapter_path.u16string()));
            insert.bind_text(4, utf8::utf16to8(run.output_model_path.u16string()));
            insert.bind_text(5, run.completed_at_utc);
            insert.bind_integer(6, static_cast<sqlite3_int64>(run.record_count));
            insert.bind_integer(7, static_cast<sqlite3_int64>(run.cumulative_record_count));
            insert.bind_integer(8, run.optimizer_steps);
            insert.bind_integer(9, run.rank);
            insert.bind_real(10, run.alpha);
            insert.bind_real(11, run.dropout);
            insert.bind_text(12, utf8::utf16to8(run.target_modules));
            if (run.training_request_json.empty()) {
                throw std::invalid_argument("LoRA training request is missing");
            }
            insert.bind_text(13, run.training_request_json);
            insert.execute();
            const auto run_id = sqlite3_last_insert_rowid(database.get());

            if (run.record_count != trained_event_ids.size()) {
                throw std::runtime_error("LoRA run record count mismatch");
            }
            const std::string eligible_sql = std::string(eligible_commits_cte) +
                "SELECT id FROM training_commits WHERE " +
                std::string(eligible_commits_where) +
                "AND event_id=?2";
            Statement eligible(database.get(), eligible_sql.c_str());
            eligible.bind_integer(1, run.parent_id);
            Statement link(database.get(),
                "INSERT INTO lora_run_commits(run_id,commit_id) VALUES(?,?)");
            for (const auto& event_id : trained_event_ids) {
                eligible.bind_text(2, utf8::utf16to8(event_id));
                if (!eligible.next()) {
                    throw std::runtime_error("training record changed before completion");
                }
                link.bind_integer(1, run_id);
                link.bind_integer(2, sqlite3_column_int64(eligible.get(), 0));
                link.execute();
                link.reset();
                eligible.reset();
                eligible.bind_integer(1, run.parent_id);
            }
            database.execute("COMMIT");
            publish_latest_available_count();
            return true;
        } catch (...) {
            try { database.execute("ROLLBACK"); } catch (...) {}
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to complete LoRA training: "
                  << error.what() << '\n';
        return false;
    }
}

void TrainingDataWriter::worker_main() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        Database database(database_path_);
        std::optional<commit_crypto::PublicParameters> public_parameters;
        std::uint64_t public_generation = 0;
        Statement insert(
            database.get(),
            "INSERT OR IGNORE INTO training_commits("
            "event_id, schema_version, context, answer, padding_json, reading, revice, "
            "event_type, committed_at_utc, revision_of, training_state) "
            "VALUES (?, 2, ?, ?, ?, ?, ?, 'commit', ?, NULL, 'pending')");

        struct StagedCommit {
            RawCommitEvent event;
            std::chrono::steady_clock::time_point deadline;
            std::uint64_t generation = 0;
        };
        std::list<StagedCommit> staged;

        const auto persist = [&](RawCommitEvent event, std::uint64_t generation) {
            try {
                if (!has_bopomofo_input(event)) return;
                std::lock_guard protection_lock(protection_mutex_);
                if (!recording_enabled_.load(std::memory_order_acquire)) return;
                if (generation != generation_.load(std::memory_order_acquire)) return;
                if (public_generation != generation) {
                    public_parameters.reset();
                    public_generation = generation;
                }
                if (!public_parameters) public_parameters = read_parameters(database);
                const auto& parameters = *public_parameters;
                const std::string event_id =
                    event.session_id + ":" + std::to_string(event.sequence);
                insert.bind_text(1, event_id);
                insert.bind_text(2, commit_crypto::seal(
                    utf8::utf16to8(event.context), event_id + "/context", parameters));
                insert.bind_text(3, commit_crypto::seal(
                    utf8::utf16to8(event.answer), event_id + "/answer", parameters));
                insert.bind_text(4, commit_crypto::seal(
                    serialize_padding(event), event_id + "/padding_json", parameters));
                insert.bind_text(5, commit_crypto::seal(
                    utf8::utf16to8(display_reading(event)),
                    event_id + "/reading", parameters));
                insert.bind_integer(6, was_revised(event) ? 1 : 0);
                insert.bind_text(7, event.committed_at_utc);
                insert.execute();
                const auto changed = sqlite3_changes(database.get());
                insert.reset();
                publish_pending_delta(static_cast<std::ptrdiff_t>(changed));
            } catch (const std::exception& error) {
                try {
                    insert.reset();
                } catch (...) {
                }
                std::cerr << "[ERR] unable to insert training data: "
                          << error.what() << '\n';
            }
        };

        while (true) {
            std::optional<QueuedOperation> operation;
            bool finish = false;
            {
                std::unique_lock lock(mutex_);
                while (queue_.empty() && !stopping_) {
                    if (staged.empty()) {
                        available_.wait(lock, [this] {
                            return stopping_ || !queue_.empty();
                        });
                    } else {
                        available_.wait_until(lock, staged.front().deadline);
                        if (std::chrono::steady_clock::now() >=
                            staged.front().deadline) {
                            break;
                        }
                    }
                }
                if (!queue_.empty()) {
                    operation = std::move(queue_.front());
                    queue_.pop_front();
                } else if (stopping_) {
                    finish = true;
                }
            }

            if (operation) {
                if (operation->kind == QueuedOperation::Kind::clear) {
                    staged.clear();
                    continue;
                }

                if (operation->kind == QueuedOperation::Kind::discard) {
                    const auto found = std::find_if(
                        staged.begin(), staged.end(), [&](const auto& candidate) {
                            return candidate.event.session_id == operation->session_id &&
                                   candidate.event.sequence == operation->sequence;
                        });
                    if (found != staged.end() &&
                        operation->queued_at <= found->deadline) {
                        staged.erase(found);
                    }
                    continue;
                }

                auto previous = std::find_if(
                    staged.begin(), staged.end(), [&](const auto& candidate) {
                        return candidate.event.session_id ==
                               operation->event.session_id;
                    });
                if (previous != staged.end()) {
                    auto event = std::move(previous->event);
                    const auto generation = previous->generation;
                    staged.erase(previous);
                    persist(std::move(event), generation);
                }
                if (recording_enabled_.load(std::memory_order_acquire) &&
                    has_bopomofo_input(operation->event)) {
                    staged.push_back(StagedCommit{
                        .event = std::move(operation->event),
                        .deadline = operation->queued_at + staging_window_,
                        .generation = operation->generation,
                    });
                }
                continue;
            }

            if (finish) {
                for (auto& pending : staged) {
                    persist(std::move(pending.event), pending.generation);
                }
                break;
            }

            const auto now = std::chrono::steady_clock::now();
            while (!staged.empty() && staged.front().deadline <= now) {
                auto event = std::move(staged.front().event);
                const auto generation = staged.front().generation;
                staged.pop_front();
                persist(std::move(event), generation);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERR] training data writer: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[ERR] training data writer failed\n";
    }
}

}  // namespace llavon::service
