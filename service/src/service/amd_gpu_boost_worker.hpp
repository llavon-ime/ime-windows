#pragma once

#include "gpu_activity_lease.hpp"

#include <optional>
#include <string_view>

namespace llavon::service {

// The driver lives in a hidden child of the current executable. The child watches
// an inherited process handle, so a killed service still releases the clock mode.
using AmdBoostDriverFactory = GpuActivityLease::Boost (*)(std::string_view);
GpuActivityLease::Boost make_amd_boost_worker(std::string_view device_id);
std::optional<int> run_amd_boost_worker(int argc, char** argv,
                                       AmdBoostDriverFactory factory) noexcept;

} // namespace llavon::service
