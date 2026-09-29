#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace llavon::debug {

enum class LogInformation : std::uint8_t {
    general = 0,
    context = 1,
};

class Logger final {
public:
    using MessageFactory = std::move_only_function<std::string()>;

    explicit Logger(std::string source);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(LogInformation information, std::string message) noexcept;

    // The factory is never evaluated on the calling thread. If the debugger is
    // disconnected or the bounded queue rejects the message, it is not
    // evaluated at all.
    void log(LogInformation information, MessageFactory make_message) noexcept;

    // Compatibility overloads classify messages as ordinary diagnostics.
    void log(std::string message) noexcept {
        log(LogInformation::general, std::move(message));
    }
    void log(MessageFactory make_message) noexcept {
        log(LogInformation::general, std::move(make_message));
    }

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace llavon::debug
