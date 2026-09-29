#pragma once

#include <ime-core/core.hpp>
#include <windows.h>
#include <optional>

namespace llavon::service {

// Some AMD iGPUs omit VK_EXT_pci_bus_info. Resolve the unique Vulkan device's
// Windows LUID instead; a Vulkan ordinal is never treated as a DXGI ordinal.
std::optional<LUID> amd_vulkan_luid(const llavon::ime::core::InferenceDeviceInfo& device);

} // namespace llavon::service
