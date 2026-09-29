#pragma once

#include <filesystem>
#include <functional>
#include <span>
#include <string>

namespace llavon::service::ryzen_ai::detail {

std::filesystem::path compiler_executable();
void run_compiler_process(const std::filesystem::path& executable,
                          std::span<const std::wstring> arguments,
                          const std::function<void(std::string)>& log_output);

} // namespace llavon::service::ryzen_ai::detail
