#pragma once

#include "user_settings.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace llavon::service::user_settings_codec {

std::optional<UserSettings> decode(std::string_view json);
std::string encode(const UserSettings& settings);

}  // namespace llavon::service::user_settings_codec
