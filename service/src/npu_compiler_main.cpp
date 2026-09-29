#include <ime-core/ryzen_ai.hpp>
#include <ime-core/logger.hpp>

#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class InheritedPipeLogger final : public llavon::ime::core::Logger {
public:
    InheritedPipeLogger() noexcept : pipe_(GetStdHandle(STD_ERROR_HANDLE)) {}

    void log(std::string message) noexcept override {
        write(message);
        write("\n");
    }

    void log(MessageFactory) noexcept override {
        // The compiler backend submits materialized messages. Reject lazy
        // factories instead of invoking them synchronously.
    }

private:
    void write(std::string_view message) noexcept {
        auto* cursor = message.data();
        auto remaining = message.size();
        while (valid() && remaining != 0) {
            DWORD written = 0;
            const auto chunk = static_cast<DWORD>(
                (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            if (!WriteFile(pipe_, cursor, chunk, &written, nullptr) || written == 0) return;
            cursor += written;
            remaining -= written;
        }
    }

    bool valid() const noexcept { return pipe_ && pipe_ != INVALID_HANDLE_VALUE; }
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    auto logger = std::make_shared<InheritedPipeLogger>();
    try {
        if (argc != 4) throw std::invalid_argument("Expected model directory, cache directory and context length");
        size_t consumed = 0;
        const auto length = std::stoull(argv[3], &consumed);
        if (consumed != std::wstring_view(argv[3]).size() ||
            length > std::numeric_limits<std::uint32_t>::max()) {
            throw std::invalid_argument("Invalid NPU context length");
        }
        auto accelerator = llavon::ime::core::create_ryzen_ai_accelerator(
            {.model_directory = argv[1], .cache_directory = argv[2], .logger = logger});
        accelerator->prepare({}, static_cast<std::uint32_t>(length));
        accelerator.reset();
        accelerator = llavon::ime::core::create_ryzen_ai_accelerator(
            {.model_directory = argv[1], .cache_directory = argv[2],
             .allow_compilation = false, .logger = logger});
        accelerator->prepare({}, static_cast<std::uint32_t>(length));
        // Session construction does not execute AMD's dynamically selected
        // attention transactions. Verify prefill, decode and rewind before the
        // service is allowed to load the compiled context.
        auto context = accelerator->create_context();
        const auto started = std::chrono::steady_clock::now();
        context->decode(std::array<std::int32_t, 3>{1, 1, 1}, 0);
        const auto vocabulary = context->logits().size();
        if (vocabulary == 0) throw std::runtime_error("NPU smoke test returned no logits");
        const auto expected = std::vector<float>(context->logits().begin(), context->logits().end());
        context->decode(std::array<std::int32_t, 1>{1}, 3);
        context->truncate(1);
        context->decode(std::array<std::int32_t, 2>{1, 1}, 1);
        const auto actual = context->logits();
        if (actual.size() != expected.size()) {
            throw std::runtime_error("NPU rewind changed logits dimensions");
        }
        double dot = 0.0;
        double actual_norm = 0.0;
        double expected_norm = 0.0;
        for (std::size_t i = 0; i < actual.size(); ++i) {
            dot += static_cast<double>(actual[i]) * expected[i];
            actual_norm += static_cast<double>(actual[i]) * actual[i];
            expected_norm += static_cast<double>(expected[i]) * expected[i];
        }
        const auto similarity = dot / std::sqrt(actual_norm * expected_norm);
        // BF16 prefill and incremental decode need not be bit-identical.
        if (!std::isfinite(similarity) || similarity < 0.99) {
            throw std::runtime_error("NPU rewind produced inconsistent logits");
        }
        logger->log(std::format("[NPU] hardware smoke test passed: prefill, decode, rewind; {} logits; {} ms",
            vocabulary, std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count()));
        return 0;
    } catch (const std::exception& error) {
        logger->log(std::string("[ERR] ") + error.what());
        return 1;
    }
}
