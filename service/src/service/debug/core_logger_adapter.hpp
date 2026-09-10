#pragma once

#include <ime-core/logger.hpp>
#include <llavon-debug/logger.hpp>

#include <string>
#include <utility>

namespace llavon::service::debug {

class CoreLoggerAdapter final : public llavon::ime::core::Logger {
public:
    CoreLoggerAdapter() : logger_("service") {}

    void log(std::string message) noexcept override {
        logger_.log(std::move(message));
    }

    void log(MessageFactory make_message) noexcept override {
        logger_.log(std::move(make_message));
    }

private:
    llavon::debug::Logger logger_;
};

}  // namespace llavon::service::debug
