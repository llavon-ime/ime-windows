#pragma once

#include <cstdint>

namespace llavon::debug::pipe_protocol {

inline constexpr const wchar_t* pipe_name = L"\\\\.\\pipe\\llavon-ime-debugger";
inline constexpr std::uint32_t maximum_message_size = 1024 * 1024;

}  // namespace llavon::debug::pipe_protocol
