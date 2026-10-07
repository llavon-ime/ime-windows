#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace llavon::service {

// Runs an owned process tree. Cancellation and callback exceptions terminate
// the entire tree; a service crash does so through the kill-on-close job.
std::uint32_t run_lora_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::atomic_bool& cancelling,
    const std::function<void(std::string_view)>& on_line);

} // namespace llavon::service
