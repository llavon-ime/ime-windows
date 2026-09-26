#include "amd_gpu_latency_boost.hpp"

#include <windows.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace llavon::service {
namespace {

// The slots and signatures below match AMD ADLX 2.0's published C ABI
// (GPUOpen-LibrariesAndSDKs/ADLX, commit 32b5a740). ADLX is loaded only at
// runtime, so builds and non-AMD machines have no SDK or driver dependency.
namespace adlx {
using Result = int;
constexpr Result ok = 0;
constexpr std::uint64_t version = (2ull << 48) | 125ull;
constexpr std::size_t mapping_get_gpu_slot = 0;
constexpr std::size_t release_slot = 1;
constexpr std::size_t query_interface_slot = 2;
constexpr std::size_t system_gpu_tuning_slot = 8;
constexpr std::size_t tuning_supported_manual_gfx_slot = 8;
constexpr std::size_t tuning_get_manual_gfx_slot = 14;
constexpr std::size_t manual_min_range_slot = 3;
constexpr std::size_t manual_get_min_slot = 4;
constexpr std::size_t manual_set_min_slot = 5;
constexpr std::size_t manual_get_max_slot = 7;

struct IntRange {
    std::int32_t minValue;
    std::int32_t maxValue;
    std::int32_t step;
};
static_assert(sizeof(IntRange) == 12);
static_assert(sizeof(void*) == 8);

using Initialize = Result(__cdecl*)(std::uint64_t, void**, void**);
using Terminate = Result(__cdecl*)();
using Release = long(__stdcall*)(void*);
using MapGpu = Result(__stdcall*)(void*, int, int, int, void**);
using GetService = Result(__stdcall*)(void*, void**);
using IsSupported = Result(__stdcall*)(void*, void*, bool*);
using GetManual = Result(__stdcall*)(void*, void*, void**);
using QueryInterface = Result(__stdcall*)(void*, const wchar_t*, void**);
using GetRange = Result(__stdcall*)(void*, IntRange*);
using GetFrequency = Result(__stdcall*)(void*, std::int32_t*);
using SetFrequency = Result(__stdcall*)(void*, std::int32_t);

template <typename Function, typename... Args>
auto call(void* object, std::size_t slot, Args... args) noexcept {
    const auto vtable = *static_cast<void***>(object);
    return reinterpret_cast<Function>(vtable[slot])(object, args...);
}
} // namespace adlx

struct PciAddress {
    int bus;
    int device;
    int function;
};

std::optional<PciAddress> pci_address(std::string_view id) noexcept {
    std::array<unsigned, 4> address{};
    constexpr std::array separators{':', ':', '.'};
    for (std::size_t i = 0; i < address.size(); ++i) {
        const auto length = i < separators.size() ? id.find(separators[i]) : id.size();
        if (length == std::string_view::npos || length == 0) return std::nullopt;
        const auto [end, error] =
            std::from_chars(id.data(), id.data() + length, address[i], 16);
        if (error != std::errc{} || end != id.data() + length) return std::nullopt;
        id.remove_prefix(length + (i < separators.size() ? 1 : 0));
    }
    if (address[0] != 0 || address[1] > 255 || address[2] > 31 || address[3] > 7) {
        return std::nullopt;
    }
    return PciAddress{static_cast<int>(address[1]), static_cast<int>(address[2]),
                      static_cast<int>(address[3])};
}

struct AdlxRelease {
    void operator()(void* pointer) const noexcept {
        if (pointer) (void)adlx::call<adlx::Release>(pointer, adlx::release_slot);
    }
};

using AdlxPtr = std::unique_ptr<void, AdlxRelease>;

class AdlxModule final {
public:
    AdlxModule()
        : handle_(LoadLibraryExW(L"amdadlx64.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!handle_) throw std::runtime_error("AMD ADLX driver library is missing");
    }
    ~AdlxModule() { (void)FreeLibrary(handle_); }

    AdlxModule(const AdlxModule&) = delete;
    AdlxModule& operator=(const AdlxModule&) = delete;

    HMODULE get() const noexcept { return handle_; }

private:
    HMODULE handle_;
};

class AdlxRuntime final {
public:
    AdlxRuntime() {
        const auto initialize = reinterpret_cast<adlx::Initialize>(
            GetProcAddress(module_.get(), "ADLXInitialize2"));
        terminate_ = reinterpret_cast<adlx::Terminate>(
            GetProcAddress(module_.get(), "ADLXTerminate"));
        if (!initialize || !terminate_) {
            throw std::runtime_error("AMD ADLX entry points are missing");
        }
        const auto result = initialize(adlx::version, &system_, &mapping_);
        if (result != adlx::ok || !system_ || !mapping_) {
            if (result == adlx::ok) (void)terminate_();
            throw std::runtime_error("AMD ADLX initialization failed");
        }
    }

    ~AdlxRuntime() {
        (void)terminate_();
    }

    AdlxRuntime(const AdlxRuntime&) = delete;
    AdlxRuntime& operator=(const AdlxRuntime&) = delete;

    void* system() const noexcept { return system_; }
    void* mapping() const noexcept { return mapping_; }

private:
    AdlxModule module_;
    adlx::Terminate terminate_ = nullptr;
    void* system_ = nullptr;
    void* mapping_ = nullptr;
};

class AmdLatencyBoost final {
public:
    explicit AmdLatencyBoost(PciAddress address) {
        void* gpu = nullptr;
        const auto mapped = adlx::call<adlx::MapGpu>(
            runtime_.mapping(), adlx::mapping_get_gpu_slot,
            address.bus, address.device, address.function, &gpu);
        gpu_.reset(gpu);
        if (mapped != adlx::ok || !gpu_) {
            throw std::runtime_error("inference GPU is not an ADLX device");
        }

        void* service = nullptr;
        const auto service_result = adlx::call<adlx::GetService>(
            runtime_.system(), adlx::system_gpu_tuning_slot, &service);
        tuning_service_.reset(service);
        if (service_result != adlx::ok || !tuning_service_) {
            throw std::runtime_error("AMD GPU tuning is unavailable");
        }

        bool supported = false;
        if (adlx::call<adlx::IsSupported>(tuning_service_.get(),
                                           adlx::tuning_supported_manual_gfx_slot,
                                           gpu_.get(), &supported) != adlx::ok ||
            !supported) {
            throw std::runtime_error("AMD manual GPU tuning is unsupported");
        }

        void* generic = nullptr;
        const auto manual_result = adlx::call<adlx::GetManual>(
            tuning_service_.get(), adlx::tuning_get_manual_gfx_slot,
            gpu_.get(), &generic);
        const AdlxPtr generic_holder(generic);
        if (manual_result != adlx::ok || !generic_holder) {
            throw std::runtime_error("AMD manual GPU tuning interface is unavailable");
        }
        void* tuning = nullptr;
        const auto query_result = adlx::call<adlx::QueryInterface>(
            generic, adlx::query_interface_slot,
            L"IADLXManualGraphicsTuning2", &tuning);
        tuning_.reset(tuning);
        if (query_result != adlx::ok || !tuning_) {
            throw std::runtime_error("AMD minimum clock tuning is unsupported");
        }
    }

    ~AmdLatencyBoost() {
        if (active_) (void)set(false);
    }

    AmdLatencyBoost(const AmdLatencyBoost&) = delete;
    AmdLatencyBoost& operator=(const AmdLatencyBoost&) = delete;

    bool set(bool enabled) noexcept {
        if (enabled) {
            adlx::IntRange range{};
            std::int32_t current_min = 0;
            std::int32_t current_max = 0;
            if (adlx::call<adlx::GetRange>(tuning_.get(), adlx::manual_min_range_slot,
                                            &range) != adlx::ok ||
                adlx::call<adlx::GetFrequency>(tuning_.get(), adlx::manual_get_min_slot,
                                                &current_min) != adlx::ok ||
                adlx::call<adlx::GetFrequency>(tuning_.get(), adlx::manual_get_max_slot,
                                                &current_max) != adlx::ok ||
                range.step <= 0) {
                return false;
            }

            const auto ceiling = std::min(range.maxValue, current_max);
            if (current_min >= ceiling || current_min < range.minValue) return false;
            const auto desired = static_cast<std::int64_t>(current_min) +
                                 (static_cast<std::int64_t>(ceiling) - current_min) * 3 / 4;
            const auto steps = (desired - range.minValue) / range.step;
            const auto target = static_cast<std::int32_t>(range.minValue + steps * range.step);
            if (target <= current_min) return false;

            const auto result = adlx::call<adlx::SetFrequency>(
                tuning_.get(), adlx::manual_set_min_slot, target);
            if (result != adlx::ok) {
                // ADLX_RESET_NEEDED means another tuning mode is in use. Do not
                // reset the user's GPU settings merely to enable this hint.
                std::clog << "[WARN] AMD GPU latency boost enable failed: adlx_status="
                          << result << '\n';
                return false;
            }
            previous_min_ = current_min;
            boosted_min_ = target;
            active_ = true;
            return true;
        }

        if (!active_) return true;
        std::int32_t current_min = 0;
        if (adlx::call<adlx::GetFrequency>(tuning_.get(), adlx::manual_get_min_slot,
                                            &current_min) != adlx::ok) return false;
        if (current_min == boosted_min_ &&
            adlx::call<adlx::SetFrequency>(tuning_.get(), adlx::manual_set_min_slot,
                                            previous_min_) != adlx::ok) {
            std::clog << "[WARN] AMD GPU latency boost restore failed\n";
            return false;
        }
        // A user or another application may have changed tuning meanwhile.
        // Never overwrite a value that is no longer ours.
        active_ = false;
        return true;
    }

private:
    AdlxRuntime runtime_;
    AdlxPtr gpu_;
    AdlxPtr tuning_service_;
    AdlxPtr tuning_;
    std::int32_t previous_min_ = 0;
    std::int32_t boosted_min_ = 0;
    bool active_ = false;
};

} // namespace

GpuActivityLease::Boost make_amd_gpu_latency_boost(std::string_view device_id) noexcept {
    const auto address = pci_address(device_id);
    if (!address) return {};
    try {
        auto boost = std::make_unique<AmdLatencyBoost>(*address);
        std::clog << "[SRV] AMD GPU latency boost available: device=" << device_id
                  << " idle_timeout_ms=2000\n";
        return [boost = std::move(boost)](bool enabled) { return boost->set(enabled); };
    } catch (const std::exception& error) {
        std::clog << "[WARN] AMD GPU latency boost unavailable: " << error.what() << '\n';
        return {};
    } catch (...) {
        std::clog << "[WARN] AMD GPU latency boost unavailable\n";
        return {};
    }
}

} // namespace llavon::service
