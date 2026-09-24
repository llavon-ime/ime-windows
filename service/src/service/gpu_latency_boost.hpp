#pragma once

#include "gpu_activity_lease.hpp"

#include <ime-core/core.hpp>

namespace llavon::service {

// Optional, device-scoped hint. An unavailable driver must not prevent input.
GpuActivityLease::Boost make_gpu_latency_boost(
    const llavon::ime::core::InferenceRuntimeInfo& runtime) noexcept;

} // namespace llavon::service
