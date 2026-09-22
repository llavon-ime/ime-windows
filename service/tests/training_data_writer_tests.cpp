#include "service/training_data_writer.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <string>

using llavon::service::RawCommitEvent;
using llavon::service::RawCommitInputEntry;
using llavon::service::TrainingDataWriter;
using llavon::service::serialize_training_data_event;

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
    const auto json = serialize_training_data_event(event);
    if (json.find(R"("context":"sample")") == std::string::npos ||
        json.find(R"("license")") != std::string::npos ||
        json.find(R"("tone":3)") == std::string::npos ||
        json.find(R"("tone":1)") == std::string::npos ||
        json.find(R"({"literal":",")") == std::string::npos ||
        json.find(R"("revice":true)") == std::string::npos ||
        json.find(R"("eventId":"session:7")") == std::string::npos ||
        json.find(R"("revisionOf":null)") == std::string::npos) {
        return 1;
    }

    const RawCommitEvent escaped{
        .context = u"a\n\"b",
        .answer = u"x",
        .input = {RawCommitInputEntry{.reading = u"\u3105", .output = u"x"}},
        .session_id = "s",
        .sequence = 1,
        .committed_at_utc = "t",
    };
    const auto escaped_json = serialize_training_data_event(escaped);
    if (escaped_json.find(R"("context":"a\n\"b")") == std::string::npos ||
        escaped_json.find(R"({"rawReading":)") == std::string::npos ||
        escaped_json.find(R"("revice":false)") == std::string::npos) {
        return 2;
    }

    std::wstring temporary_directory(MAX_PATH, L'\0');
    const DWORD temporary_length = GetTempPathW(
        static_cast<DWORD>(temporary_directory.size()), temporary_directory.data());
    if (temporary_length == 0 || temporary_length >= temporary_directory.size()) return 3;
    temporary_directory.resize(temporary_length);
    const auto path = std::filesystem::path(temporary_directory) /
                      (L"llavon-training-data-writer-test-" +
                       std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    DeleteFileW(path.c_str());
    {
        TrainingDataWriter writer(path);
        writer.enqueue(event);
        writer.enqueue(escaped);
    }
    std::ifstream input(path, std::ios::binary);
    const std::string body{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    DeleteFileW(path.c_str());
    if (body.find("\n") == std::string::npos ||
        body.find(R"("eventId":"session:7")") == std::string::npos ||
        body.find(R"("eventId":"s:1")") == std::string::npos) {
        return 4;
    }
    return 0;
}
