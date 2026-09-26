#pragma once

#include "gpu_activity_lease.hpp"

#include <string_view>

namespace llavon::service {

GpuActivityLease::Boost make_amd_gpu_latency_boost(std::string_view device_id) noexcept;

} // namespace llavon::service
