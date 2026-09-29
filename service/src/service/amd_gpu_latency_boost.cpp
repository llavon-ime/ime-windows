#include "amd_gpu_latency_boost.hpp"
#include "amd_gpu_boost_worker.hpp"
#include "amd_gpu_pci_address.hpp"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace llavon::service {
namespace {

// AMD CLR's opencl/amdocl/cl_profile_amd.h and khronos/headers/opencl2.2/CL/cl_ext.h:
// https://github.com/ROCm/clr/blob/develop/opencl/amdocl/cl_profile_amd.h
// This profiling extension reaches PAL::IDevice::SetClockMode. It needs neither
// an OpenCL context nor submitted work, and also affects Vulkan inference.
namespace cl {
using Int = std::int32_t;
using UInt = std::uint32_t;
using Id = void*;
using Platforms = Int(__stdcall*)(UInt, Id*, UInt*);
using Devices = Int(__stdcall*)(Id, std::uint64_t, UInt, Id*, UInt*);
using Info = Int(__stdcall*)(Id, UInt, std::size_t, void*, std::size_t*);
using Extension = void*(__stdcall*)(Id, const char*);
enum class Mode : Int { normal = 0, query = 1, peak = 5 };
struct Input { Mode mode; };
// Drivers differ in output units (ratios versus clocks). Treat these bytes as
// opaque; neither target selection nor capability checks depend on those units.
struct Output { std::array<std::uint32_t, 2> opaque{}; };
using ClockMode = Int(__stdcall*)(Id, Input, Output*);
struct Topology {
    UInt type;
    std::array<unsigned char, 17> unused;
    unsigned char bus;
    unsigned char device;
    unsigned char function;
};
static_assert(sizeof(Input) == 4 && sizeof(Output) == 8 && sizeof(Topology) == 24);
static_assert(offsetof(Topology, bus) == 21);
constexpr UInt device_vendor_id = 0x1001;
constexpr UInt device_topology_amd = 0x4037;
constexpr UInt topology_pcie = 1;
constexpr Int success = 0;
constexpr Int invalid_operation = -59;
constexpr Int invalid_device = -33;
} // namespace cl

struct ModuleRelease {
    void operator()(HINSTANCE__* module) const noexcept { if (module) FreeLibrary(module); }
};

class AmdDriverClock final {
public:
    explicit AmdDriverClock(AmdPciAddress address)
        : module_(LoadLibraryExW(L"OpenCL.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module_) throw std::runtime_error("AMD OpenCL driver loader is missing");
        const auto platforms = entry<cl::Platforms>("clGetPlatformIDs");
        const auto devices = entry<cl::Devices>("clGetDeviceIDs");
        const auto info = entry<cl::Info>("clGetDeviceInfo");
        const auto extension = entry<cl::Extension>("clGetExtensionFunctionAddressForPlatform");
        std::array<cl::Id, 32> platform_ids{};
        cl::UInt platform_count = 0;
        if (platforms(static_cast<cl::UInt>(platform_ids.size()), platform_ids.data(),
                      &platform_count) != cl::success || platform_count > platform_ids.size()) {
            throw std::runtime_error("OpenCL platforms are unavailable");
        }
        for (cl::UInt p = 0; p < platform_count; ++p) {
            const auto clock = reinterpret_cast<cl::ClockMode>(
                extension(platform_ids[p], "clSetDeviceClockModeAMD"));
            if (!clock) continue;
            std::array<cl::Id, 64> device_ids{};
            cl::UInt count = 0;
            constexpr std::uint64_t gpu_device_type = 4;
            if (devices(platform_ids[p], gpu_device_type, static_cast<cl::UInt>(device_ids.size()),
                        device_ids.data(), &count) != cl::success || count > device_ids.size()) continue;
            for (cl::UInt i = 0; i < count; ++i) {
                cl::UInt vendor = 0;
                cl::Topology topology{};
                if (info(device_ids[i], cl::device_vendor_id, sizeof(vendor), &vendor, nullptr) !=
                        cl::success || vendor != 0x1002 ||
                    info(device_ids[i], cl::device_topology_amd, sizeof(topology), &topology, nullptr) !=
                        cl::success || topology.type != cl::topology_pcie ||
                    AmdPciAddress{topology.bus, topology.device, topology.function} != address) continue;
                cl::Output output{};
                if (clock(device_ids[i], {cl::Mode::query}, &output) != cl::success) continue;
                device_ = device_ids[i];
                clock_ = clock;
                return;
            }
        }
        throw std::runtime_error("inference GPU does not expose AMD driver clock-mode control");
    }

    ~AmdDriverClock() { if (restore_pending_) (void)set(false); }
    AmdDriverClock(const AmdDriverClock&) = delete;
    AmdDriverClock& operator=(const AmdDriverClock&) = delete;

    GpuActivityLease::Result set(bool enabled) noexcept {
        using Result = GpuActivityLease::Result;
        if (!enabled && !restore_pending_) return Result::success;
        // Even a failed Peak request may have partially changed driver state.
        if (enabled) restore_pending_ = true;
        cl::Output output{};
        const auto status = clock_(device_, {enabled ? cl::Mode::peak : cl::Mode::normal}, &output);
        if (status != cl::success) {
            std::clog << "[WARN] AMD driver clock mode failed: mode=" << (enabled ? "Peak" : "Default")
                      << " cl_status=" << status << '\n';
            return enabled && (status == cl::invalid_operation || status == cl::invalid_device)
                ? Result::unavailable : Result::retry_later;
        }
        if (!enabled) restore_pending_ = false;
        return Result::success;
    }

private:
    template <typename Function>
    Function entry(const char* name) const {
        const auto function = reinterpret_cast<Function>(GetProcAddress(module_.get(), name));
        if (!function) throw std::runtime_error("OpenCL entry point is missing");
        return function;
    }
    std::unique_ptr<HINSTANCE__, ModuleRelease> module_;
    cl::Id device_ = nullptr;
    cl::ClockMode clock_ = nullptr;
    bool restore_pending_ = false;
};

GpuActivityLease::Boost make_driver(std::string_view device_id) {
    const auto address = amd_pci_address(device_id);
    if (!address) return {};
    auto driver = std::make_unique<AmdDriverClock>(*address);
    return [driver = std::move(driver)](bool enabled) { return driver->set(enabled); };
}

} // namespace

GpuActivityLease::Boost make_amd_gpu_latency_boost(std::string_view device_id) noexcept {
    if (!amd_pci_address(device_id)) return {};
    try {
        auto boost = make_amd_boost_worker(device_id);
        std::clog << "[SRV] AMD GPU latency boost available: device=" << device_id
                  << " api=clSetDeviceClockModeAMD mode=Peak idle_timeout_ms=2000\n";
        return boost;
    } catch (const std::exception& error) {
        std::clog << "[WARN] AMD GPU latency boost unavailable: " << error.what() << '\n';
    } catch (...) {
        std::clog << "[WARN] AMD GPU latency boost unavailable\n";
    }
    return {};
}

std::optional<int> run_amd_gpu_latency_boost_worker(int argc, char** argv) noexcept {
    return run_amd_boost_worker(argc, argv, make_driver);
}

} // namespace llavon::service
