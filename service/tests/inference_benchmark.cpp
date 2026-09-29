#include <ime-core/core.hpp>
#include <ime-core/logger.hpp>
#include <ime-core/ryzen_ai.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace core = llavon::ime::core;
namespace {
class BenchmarkLogger final : public core::Logger {
public:
    void log(std::string) noexcept override {}
    void log(MessageFactory factory) noexcept override {
        try {
            const std::unique_lock lock(mutex_, std::try_to_lock);
            if (lock.owns_lock()) pending_.push_back(std::move(factory));
        } catch (...) {}
    }
    void log(core::LogInformation information, MessageFactory factory) noexcept override {
        if (information == core::LogInformation::general) log(std::move(factory));
    }
    void drain() {
        std::vector<MessageFactory> pending;
        {
            const std::lock_guard lock(mutex_);
            pending.swap(pending_);
        }
        // Materialize diagnostics off the inference thread, outside its timer.
        const std::jthread worker([pending = std::move(pending)] {
            for (const auto& factory : pending) {
                const auto message = factory();
                if (message.starts_with("[TIME]")) std::cout << message << '\n';
            }
        });
    }
private:
    std::mutex mutex_;
    std::vector<MessageFactory> pending_;
};

struct Request {
    std::string label;
    std::u16string context;
    std::vector<std::u16string> syllables;
};
}

// Manual benchmark, excluded from CTest. Uses the actual IME candidate pipeline.
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 4 && argc != 6) {
            throw std::invalid_argument("Expected cpu|npu GGUF tables [NPU model cache]");
        }
        const bool npu = std::wstring_view(argv[1]) == L"npu";
        if ((npu && argc != 6) || (!npu && std::wstring_view(argv[1]) != L"cpu")) {
            throw std::invalid_argument("Invalid benchmark backend/arguments");
        }
        const auto logger = std::make_shared<BenchmarkLogger>();
        core::CoreConfig config{
            .model_path = std::filesystem::absolute(argv[2]),
            .tables_dir = std::filesystem::absolute(argv[3]),
            .context_length = 384,
            .threads = 8,
            .inference_device = {npu ? core::InferenceBackend::ryzen_ai : core::InferenceBackend::cpu, {}},
            .logger = logger};
        if (npu) {
            config.accelerator = core::create_ryzen_ai_accelerator({
                .model_directory = std::filesystem::absolute(argv[4]),
                .cache_directory = std::filesystem::absolute(argv[5]),
                .allow_compilation = false, .logger = logger});
        }
        core::Core model(std::move(config));
        const std::array requests{
            Request{"one", u"", {u"ㄋㄧˇ"}},
            Request{"two", u"", {u"ㄋㄧˇ", u"ㄏㄠˇ"}},
            Request{"delete_tail", u"", {u"ㄋㄧˇ"}},
            Request{"replace_tail", u"", {u"ㄋㄧˇ", u"ㄇㄣ˙"}},
            Request{"new_two", u"", {u"ㄐㄧㄣ ", u"ㄊㄧㄢ "}},
            Request{"four", u"", {u"ㄐㄧㄣ ", u"ㄊㄧㄢ ", u"ㄊㄧㄢ ", u"ㄑㄧˋ"}},
            Request{"context_two", u"今天天氣", {u"ㄏㄣˇ", u"ㄏㄠˇ"}},
            Request{"reset_two", u"", {u"ㄋㄧˇ", u"ㄏㄠˇ"}}};
        std::array<std::vector<double>, requests.size()> samples;
        constexpr auto key_interval = std::chrono::milliseconds(250);
        std::cout << "CONFIG,key_interval_ms,250,measured_rounds,30\n";
        for (int round = 0; round < 31; ++round) {
            auto session = model.create_session();
            session->ready();
            logger->drain();
            auto next_request = std::chrono::steady_clock::now() + key_interval;
            for (std::size_t i = 0; i < requests.size(); ++i) {
                const auto& request = requests[i];
                std::vector<core::PaddingEntry> padding;
                for (const auto& syllable : request.syllables) padding.push_back({.bopomofo = syllable});
                std::this_thread::sleep_until(next_request);
                const auto started = std::chrono::steady_clock::now();
                next_request = started + key_interval;
                const auto result = session->predict(request.context, padding);
                const double elapsed = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count();
                if (result.size() != padding.size() || std::ranges::any_of(result,
                    [](const auto& entry) { return entry.candidates.empty(); })) {
                    throw std::runtime_error("Prediction returned no candidates");
                }
                if (round != 0) samples[i].push_back(elapsed);
                std::cout << std::format("SAMPLE,{},{},{},{:.3f}\n", npu ? "npu" : "cpu", round, request.label, elapsed);
                logger->drain();
            }
        }
        for (std::size_t i = 0; i < requests.size(); ++i) {
            std::ranges::sort(samples[i]);
            const auto count = samples[i].size();
            const double median = (samples[i][(count - 1) / 2] + samples[i][count / 2]) * .5;
            std::cout << std::format("MEDIAN,{},{},{:.3f}\n", npu ? "npu" : "cpu", requests[i].label, median);
            const auto p95 = static_cast<std::size_t>(std::ceil(samples[i].size() * .95)) - 1;
            std::cout << std::format("P95,{},{},{:.3f}\nMAX,{},{},{:.3f}\n",
                npu ? "npu" : "cpu", requests[i].label, samples[i][p95],
                npu ? "npu" : "cpu", requests[i].label, samples[i].back());
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
