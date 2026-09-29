#include "amd_vulkan_identity.hpp"

#include <vulkan/vulkan_core.h>

#include <array>
#include <charconv>
#include <cstring>
#include <memory>
#include <string_view>

namespace llavon::service {
namespace {
struct ModuleRelease {
    void operator()(HINSTANCE__* module) const noexcept { if (module) FreeLibrary(module); }
};
struct InstanceRelease {
    PFN_vkDestroyInstance destroy;
    void operator()(VkInstance instance) const noexcept { destroy(instance, nullptr); }
};
} // namespace

std::optional<LUID> amd_vulkan_luid(const llavon::ime::core::InferenceDeviceInfo& device) {
    namespace core = llavon::ime::core;
    if (device.backend != core::InferenceBackend::vulkan || device.device_id != device.name ||
        device.description.empty()) return std::nullopt;
    std::string_view name(device.name);
    if (!name.starts_with("Vulkan")) return std::nullopt;
    name.remove_prefix(6);
    unsigned ordinal = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), ordinal);
    if (error != std::errc{} || end != name.data() + name.size()) return std::nullopt;
    // Also reject a duplicate name in the inference backend's view. Neither
    // loader ordering nor a model name alone identifies one of two identical GPUs.
    unsigned inference_matches = 0;
    for (const auto& candidate : core::enumerate_inference_devices()) {
        if (candidate.backend == device.backend && candidate.description == device.description) {
            ++inference_matches;
        }
    }
    if (inference_matches != 1) return std::nullopt;

    const std::unique_ptr<HINSTANCE__, ModuleRelease> module(
        LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32));
    if (!module) return std::nullopt;
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(module.get(), "vkGetInstanceProcAddr"));
    if (!get) return std::nullopt;
    const auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
    if (!create) return std::nullopt;
    VkApplicationInfo application{};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Llavon GPU identity";
    application.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo create_info{};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &application;
    VkInstance raw = VK_NULL_HANDLE;
    if (create(&create_info, nullptr, &raw) != VK_SUCCESS) return std::nullopt;
    const auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(raw, "vkDestroyInstance"));
    // Vulkan guarantees this core command for a successfully created instance.
    const std::unique_ptr<VkInstance_T, InstanceRelease> instance(raw, InstanceRelease{destroy});
    const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(raw, "vkEnumeratePhysicalDevices"));
    const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(raw, "vkGetPhysicalDeviceProperties2"));
    if (!enumerate || !properties) return std::nullopt;
    std::array<VkPhysicalDevice, 32> devices{};
    std::uint32_t count = static_cast<std::uint32_t>(devices.size());
    if (enumerate(raw, &count, devices.data()) != VK_SUCCESS) return std::nullopt;
    std::optional<LUID> found;
    unsigned matches = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties identity{};
        identity.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        VkPhysicalDeviceProperties2 info{};
        info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        info.pNext = &identity;
        properties(devices[i], &info);
        if (info.properties.vendorID != 0x1002 || device.description != info.properties.deviceName) continue;
        if (++matches > 1) return std::nullopt;
        const bool integrated = info.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
        if (integrated != (device.type == core::InferenceDeviceType::integrated_gpu) || !identity.deviceLUIDValid) continue;
        LUID luid{};
        static_assert(sizeof(luid) == VK_LUID_SIZE);
        std::memcpy(&luid, identity.deviceLUID, sizeof(luid));
        found = luid;
    }
    return found;
}

} // namespace llavon::service
