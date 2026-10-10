#pragma once

#include <ime-core/logger.hpp>
#include <llavon-debug/logger.hpp>

#include <string>
#include <utility>

namespace llavon::service::debug {

class CoreLoggerAdapter final : public llavon::ime::core::Logger {
public:
    explicit CoreLoggerAdapter(std::string source = "service") : logger_(std::move(source)) {}

    void log(std::string message) noexcept override {
        logger_.log(llavon::debug::LogInformation::general, std::move(message));
    }

    void log(MessageFactory make_message) noexcept override {
        logger_.log(llavon::debug::LogInformation::general,
                    std::move(make_message));
    }

    void log(llavon::ime::core::LogInformation information,
             std::string message) noexcept override {
        logger_.log(to_debug_information(information), std::move(message));
    }

    void log(llavon::ime::core::LogInformation information,
             MessageFactory make_message) noexcept override {
        logger_.log(to_debug_information(information), std::move(make_message));
    }

private:
    static constexpr llavon::debug::LogInformation to_debug_information(
        llavon::ime::core::LogInformation information) noexcept {
        switch (information) {
            case llavon::ime::core::LogInformation::debug:
                return llavon::debug::LogInformation::debug;
            case llavon::ime::core::LogInformation::context:
                return llavon::debug::LogInformation::context;
            case llavon::ime::core::LogInformation::general:
            default:
                return llavon::debug::LogInformation::general;
        }
    }

    llavon::debug::Logger logger_;
};

}  // namespace llavon::service::debug
