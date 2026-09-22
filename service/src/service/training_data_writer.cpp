#include "training_data_writer.hpp"

#include <shlobj.h>
#include <utf8/cpp20.h>
#include <windows.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace llavon::service {
namespace {

constexpr wchar_t data_path_environment[] = L"LLAVON_IME_TRAINING_DATA_PATH";
constexpr wchar_t default_directory_name[] = L"Llavon IME";
constexpr wchar_t default_data_directory_name[] = L"training-data";
constexpr wchar_t default_filename[] = L"commits.jsonl";

std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;

    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return std::filesystem::path(std::move(value));
}

std::filesystem::path default_output_path() {
    if (auto configured = environment_path(data_path_environment)) {
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

}  // namespace

std::string serialize_training_data_event(const RawCommitEvent& event) {
    std::string output;
    output.reserve(256 + event.context.size() * 3 + event.answer.size() * 3);
    output += R"({"schemaVersion":1,"context":)";
    append_json_string(output, utf8::utf16to8(event.context));
    output += R"(,"answer":)";
    append_json_string(output, utf8::utf16to8(event.answer));
    output += R"(,"padding":[)";

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
            // Preserve malformed/incomplete raw input rather than silently
            // dropping a commit. It can be filtered during dataset preparation.
            output += R"({"rawReading":)";
            append_json_string(output, utf8::utf16to8(entry.reading));
            output.push_back('}');
        }
    }

    bool revice = false;
    for (const auto& entry : event.input) {
        revice = revice || entry.manually_selected;
    }
    output += revice ? R"(],"revice":true,"eventId":)"
                      : R"(],"revice":false,"eventId":)";
    append_json_string(output, event.session_id + ":" + std::to_string(event.sequence));
    output += R"(,"eventType":"commit","committedAtUtc":)";
    append_json_string(output, event.committed_at_utc);
    // This append-only link is intentionally present before correction
    // detection exists. A later correction event can point at the commit it
    // supersedes without rewriting an existing JSONL line.
    output += R"(,"revisionOf":null})";
    return output;
}

TrainingDataWriter::TrainingDataWriter(
    std::optional<std::filesystem::path> output_path)
    : output_path_(std::move(output_path)),
      worker_([this] { worker_main(); }) {}

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

void TrainingDataWriter::worker_main() noexcept {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    try {
        const auto path = output_path_ ? *output_path_ : default_output_path();
        if (!path.parent_path().empty()) {
            std::filesystem::create_directories(path.parent_path());
        }
        std::ofstream output(path, std::ios::binary | std::ios::app);
        if (!output) {
            std::cerr << "[ERR] unable to open training data file\n";
            return;
        }

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
            output << serialize_training_data_event(event) << '\n';
            output.flush();
            if (!output) {
                std::cerr << "[ERR] unable to append training data\n";
                return;
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "[ERR] training data writer: " << error.what() << '\n';
    } catch (...) {
        std::cerr << "[ERR] training data writer failed\n";
    }
}

}  // namespace llavon::service
