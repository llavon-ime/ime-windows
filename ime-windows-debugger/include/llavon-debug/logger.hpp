#pragma once

#include <functional>
#include <memory>
#include <string>

namespace llavon::debug {

class Logger final {
public:
    using MessageFactory = std::move_only_function<std::string()>;

    explicit Logger(std::string source);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void log(std::string message) noexcept;

    // The factory is never evaluated on the calling thread. If the debugger is
    // disconnected or the bounded queue rejects the message, it is not
    // evaluated at all.
    void log(MessageFactory make_message) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace llavon::debug
