#include <ime-core/ryzen_ai.hpp>
#include <ime-core/logger.hpp>

#include <Windows.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

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
        return 0;
    } catch (const std::exception& error) {
        logger->log(std::string("[ERR] ") + error.what());
        return 1;
    }
}
