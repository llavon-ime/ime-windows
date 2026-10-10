#pragma once

#include <llavon-debug/logger.hpp>

#include <cstdint>

namespace llavon::debug::pipe_protocol {

#ifdef LLAVON_DEBUG_TEST_PIPE
inline constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime-debugger-transport-tests";
#else
inline constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime-debugger";
#endif
inline constexpr std::uint32_t maximum_message_size = 1024 * 1024;

constexpr LogInformation decode_information(std::uint64_t value) noexcept {
    switch (value) {
        case static_cast<std::uint8_t>(LogInformation::context):
            return LogInformation::context;
        case static_cast<std::uint8_t>(LogInformation::debug):
            return LogInformation::debug;
        default:
            return LogInformation::general;
    }
}

}  // namespace llavon::debug::pipe_protocol
