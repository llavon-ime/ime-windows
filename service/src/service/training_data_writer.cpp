#include "training_data_writer.hpp"

#include <shlobj.h>
#include <sqlite3.h>
#include <utf8/cpp20.h>
#include <windows.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

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

void initialize_database(const std::filesystem::path& path) {
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path());
    }
    Database database(path);
    database.execute("PRAGMA journal_mode=WAL");
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
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = sqlite3_column_text(statement, column);
    if (!value) return {};
    return std::string(reinterpret_cast<const char*>(value),
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
}

}  // namespace

TrainingDataWriter::TrainingDataWriter(
    std::optional<std::filesystem::path> database_path)
    : database_path_(database_path ? std::move(*database_path)
                                   : default_database_path()) {
    initialize_database(database_path_);
    {
        Database database(database_path_);
        Statement select(
            database.get(),
            "SELECT COUNT(*) FROM training_commits WHERE training_state='pending'");
        if (!select.next()) throw std::runtime_error("unable to count training data");
        pending_count_.store(static_cast<std::size_t>(
            sqlite3_column_int64(select.get(), 0)), std::memory_order_release);
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
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back(std::move(event));
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

std::vector<TrainingDataItem> TrainingDataWriter::pending_items() const noexcept {
    try {
        Database database(database_path_);
        Statement select(
            database.get(),
            "SELECT event_id, context, answer, reading, revice "
            "FROM training_commits WHERE training_state='pending' ORDER BY id");
        std::vector<TrainingDataItem> result;
        while (select.next()) {
            result.push_back(TrainingDataItem{
                .event_id = utf8::utf8to16(column_text(select.get(), 0)),
                .context = utf8::utf8to16(column_text(select.get(), 1)),
                .answer = utf8::utf8to16(column_text(select.get(), 2)),
                .reading = utf8::utf8to16(column_text(select.get(), 3)),
                .revice = sqlite3_column_int(select.get(), 4) != 0,
            });
        }
        return result;
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to read training data: " << error.what() << '\n';
        return {};
    }
}

std::vector<TrainingDataRecord> TrainingDataWriter::pending_records(
    const std::vector<std::u16string>& event_ids) const {
    Database database(database_path_);
    Statement select(
        database.get(),
        "SELECT event_id, context, answer, padding_json, revice "
        "FROM training_commits WHERE training_state='pending' AND event_id=?");
    std::vector<TrainingDataRecord> result;
    result.reserve(event_ids.size());
    for (const auto& event_id : event_ids) {
        select.bind_text(1, utf8::utf16to8(event_id));
        if (select.next()) {
            result.push_back(TrainingDataRecord{
                .event_id = utf8::utf8to16(column_text(select.get(), 0)),
                .context = utf8::utf8to16(column_text(select.get(), 1)),
                .answer = utf8::utf8to16(column_text(select.get(), 2)),
                .padding_json = column_text(select.get(), 3),
                .revice = sqlite3_column_int(select.get(), 4) != 0,
            });
        }
        select.reset();
    }
    return result;
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
            const auto changed = sqlite3_changes(database.get());
            database.execute("COMMIT");
            publish_pending_delta(-static_cast<std::ptrdiff_t>(changed));
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

bool TrainingDataWriter::mark_trained(
    const std::vector<std::u16string>& trained_event_ids) noexcept {
    return mark_records(trained_event_ids, "trained");
}

bool TrainingDataWriter::mark_records(
    const std::vector<std::u16string>& event_ids, const char* state) noexcept {
    try {
        Database database(database_path_);
        database.execute("BEGIN IMMEDIATE");
        try {
            database.execute(
                "CREATE TEMP TABLE IF NOT EXISTS selected_training_events ("
                "event_id TEXT PRIMARY KEY)");
            database.execute("DELETE FROM selected_training_events");
            Statement insert_selected(
                database.get(),
                "INSERT OR IGNORE INTO selected_training_events(event_id) VALUES (?)");
            for (const auto& id : event_ids) {
                insert_selected.bind_text(1, utf8::utf16to8(id));
                insert_selected.execute();
                insert_selected.reset();
            }

            Statement update(
                database.get(),
                "UPDATE training_commits SET training_state=? "
                "WHERE training_state='pending' AND event_id IN "
                "(SELECT event_id FROM selected_training_events)");
            update.bind_text(1, state);
            update.execute();
            const auto changed = sqlite3_changes(database.get());
            database.execute("COMMIT");
            publish_pending_delta(-static_cast<std::ptrdiff_t>(changed));
            return true;
        } catch (...) {
            try {
                database.execute("ROLLBACK");
            } catch (...) {
            }
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERR] unable to update training data: " << error.what() << '\n';
        return false;
    }
}

void TrainingDataWriter::worker_main() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        Database database(database_path_);
        Statement insert(
            database.get(),
            "INSERT OR IGNORE INTO training_commits("
            "event_id, schema_version, context, answer, padding_json, reading, revice, "
            "event_type, committed_at_utc, revision_of, training_state) "
            "VALUES (?, 1, ?, ?, ?, ?, ?, 'commit', ?, NULL, 'pending')");

        while (true) {
            RawCommitEvent event;
            {
                std::unique_lock lock(mutex_);
                available_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
                if (queue_.empty()) {
                    if (stopping_) break;
                    continue;
                }
                event = std::move(queue_.front());
                queue_.pop_front();
            }

            try {
                const std::string event_id =
                    event.session_id + ":" + std::to_string(event.sequence);
                insert.bind_text(1, event_id);
                insert.bind_text(2, utf8::utf16to8(event.context));
                insert.bind_text(3, utf8::utf16to8(event.answer));
                insert.bind_text(4, serialize_padding(event));
                insert.bind_text(5, utf8::utf16to8(display_reading(event)));
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
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERR] training data writer: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[ERR] training data writer failed\n";
    }
}

}  // namespace llavon::service
