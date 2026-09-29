#pragma once

#include "gpu_activity_lease.hpp"

#include <string_view>
#include <optional>

namespace llavon::service {

GpuActivityLease::Boost make_amd_gpu_latency_boost(std::string_view device_id) noexcept;
// Dispatch before the service singleton or any model/UI initialization.
std::optional<int> run_amd_gpu_latency_boost_worker(int argc, char** argv) noexcept;

} // namespace llavon::service
