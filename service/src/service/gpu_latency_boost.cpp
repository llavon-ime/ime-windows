#include "gpu_latency_boost.hpp"

#include <windows.h>
#include <d3d11.h>
#include <d3dkmthk.h>
#include <dxgi1_2.h>
#include <winrt/base.h>

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

constexpr std::uint32_t nvidia_vendor_id = 0x10de;

struct ModuleHandleTraits {
    using type = HMODULE;

    static void close(type module) noexcept { (void)FreeLibrary(module); }
    static constexpr type invalid() noexcept { return nullptr; }
};

using ModuleHandle = winrt::handle_type<ModuleHandleTraits>;

struct D3dKmtAdapterHandleTraits {
    using type = D3DKMT_HANDLE;

    static void close(type adapter) noexcept {
        D3DKMT_CLOSEADAPTER request{};
        request.hAdapter = adapter;
        (void)D3DKMTCloseAdapter(&request);
    }

    static constexpr type invalid() noexcept { return 0; }
};

using D3dKmtAdapterHandle = winrt::handle_type<D3dKmtAdapterHandleTraits>;

std::optional<std::array<unsigned, 4>> pci_address(std::string_view id) noexcept {
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

    // Windows' adapter address query has no PCI-domain field. Do not guess on
    // an address we cannot match unambiguously.
    if (address[0] != 0 || address[1] > 255 || address[2] > 31 || address[3] > 7) {
        return std::nullopt;
    }
    return address;
}

bool matches_adapter(LUID luid, const std::array<unsigned, 4>& expected) noexcept {
    D3DKMT_OPENADAPTERFROMLUID request{};
    request.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&request) < 0) return false;

    const D3dKmtAdapterHandle adapter{request.hAdapter};
    D3DKMT_ADAPTERADDRESS address{};
    D3DKMT_QUERYADAPTERINFO query{};
    query.hAdapter = adapter.get();
    query.Type = KMTQAITYPE_ADAPTERADDRESS;
    query.pPrivateDriverData = &address;
    query.PrivateDriverDataSize = sizeof(address);

    return D3DKMTQueryAdapterInfo(&query) >= 0 && address.BusNumber == expected[1] &&
           address.DeviceNumber == expected[2] && address.FunctionNumber == expected[3];
}

winrt::com_ptr<IDXGIAdapter1> find_adapter(std::string_view device_id) {
    const auto address = pci_address(device_id);
    if (!address) return {};

    winrt::com_ptr<IDXGIFactory1> factory;
    winrt::check_hresult(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.put_void()));

    for (UINT index = 0;; ++index) {
        winrt::com_ptr<IDXGIAdapter1> adapter;
        const auto result = factory->EnumAdapters1(index, adapter.put());
        if (result == DXGI_ERROR_NOT_FOUND) return {};
        winrt::check_hresult(result);

        DXGI_ADAPTER_DESC1 description{};
        winrt::check_hresult(adapter->GetDesc1(&description));
        if (description.VendorId == nvidia_vendor_id &&
            (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0 &&
            matches_adapter(description.AdapterLuid, *address)) {
            return adapter;
        }
    }
}

namespace nvapi {

using Status = int;
using QueryInterface = void*(__cdecl*)(std::uint32_t);
using Lifecycle = Status(__cdecl*)();

inline constexpr Status ok = 0;
inline constexpr std::uint32_t initialize_id = 0x0150e828;
inline constexpr std::uint32_t unload_id = 0xd22bdd7e;
inline constexpr std::uint32_t set_sleep_mode_id = 0xac1ca9e0;

// Public NVAPI ABI (Release 455+), loaded optionally from the display driver.
// NvBool is an unsigned byte. Pin the structure layout to the published V1 ABI.
struct SleepModeParams {
    std::uint32_t version = sizeof(SleepModeParams) | (1u << 16);
    std::uint8_t low_latency_mode = 0;
    std::uint8_t low_latency_boost = 0;
    std::uint32_t minimum_interval_us = 0;
    std::uint8_t use_markers = 0;
    std::array<std::uint8_t, 31> reserved{};
};

using SetSleepMode = Status(__cdecl*)(IUnknown*, SleepModeParams*);

static_assert(sizeof(SleepModeParams) == 44);
static_assert(offsetof(SleepModeParams, minimum_interval_us) == 8);
static_assert(offsetof(SleepModeParams, reserved) == 13);

template <typename Function>
Function resolve(QueryInterface query, std::uint32_t id) noexcept {
    return reinterpret_cast<Function>(query(id));
}

} // namespace nvapi

class NvApiLibrary final {
public:
    NvApiLibrary()
        : module_(LoadLibraryExW(
              L"nvapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (!module_) winrt::throw_last_error();

        const auto query = reinterpret_cast<nvapi::QueryInterface>(
            GetProcAddress(module_.get(), "nvapi_QueryInterface"));
        if (!query) winrt::throw_last_error();

        const auto initialize = nvapi::resolve<nvapi::Lifecycle>(query, nvapi::initialize_id);
        const auto unload = nvapi::resolve<nvapi::Lifecycle>(query, nvapi::unload_id);
        const auto set_sleep_mode =
            nvapi::resolve<nvapi::SetSleepMode>(query, nvapi::set_sleep_mode_id);
        if (!initialize || !unload || !set_sleep_mode) {
            throw std::runtime_error("required NVAPI entry point is unavailable");
        }
        if (initialize() != nvapi::ok) {
            throw std::runtime_error("NVAPI initialization failed");
        }
        unload_ = unload;
        set_sleep_mode_ = set_sleep_mode;
    }

    ~NvApiLibrary() {
        if (unload_) (void)unload_();
    }

    NvApiLibrary(const NvApiLibrary&) = delete;
    NvApiLibrary& operator=(const NvApiLibrary&) = delete;

    nvapi::Status set_sleep_mode(IUnknown* device,
                                 nvapi::SleepModeParams& params) const noexcept {
        return set_sleep_mode_(device, &params);
    }

private:
    ModuleHandle module_;
    nvapi::Lifecycle unload_ = nullptr;
    nvapi::SetSleepMode set_sleep_mode_ = nullptr;
};

class NvidiaLatencyBoost final {
public:
    explicit NvidiaLatencyBoost(IDXGIAdapter1* adapter) {
        // This offscreen device owns only the transient boost hint. Inference
        // stays on its existing backend; no window, swapchain or dummy work.
        winrt::check_hresult(D3D11CreateDevice(
            adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
            D3D11_SDK_VERSION, device_.put(), nullptr, nullptr));
    }

    ~NvidiaLatencyBoost() {
        if (active_) (void)set(false);
    }

    NvidiaLatencyBoost(const NvidiaLatencyBoost&) = delete;
    NvidiaLatencyBoost& operator=(const NvidiaLatencyBoost&) = delete;

    bool set(bool enabled) noexcept {
        nvapi::SleepModeParams params;
        params.low_latency_boost = enabled ? 1 : 0;
        const auto status = api_.set_sleep_mode(device_.get(), params);
        if (status != nvapi::ok) {
            std::clog << "[WARN] GPU latency boost " << (enabled ? "enable" : "disable")
                      << " failed: nvapi_status=" << status << '\n';
            return false;
        }
        active_ = enabled;
        return true;
    }

private:
    NvApiLibrary api_;
    winrt::com_ptr<ID3D11Device> device_;
    bool active_ = false;
};

} // namespace

GpuActivityLease::Boost make_gpu_latency_boost(
    const llavon::ime::core::InferenceRuntimeInfo& runtime) noexcept {
    using llavon::ime::core::InferenceBackend;
    if (!runtime.gpu_offload ||
        (runtime.device.backend != InferenceBackend::vulkan &&
         runtime.device.backend != InferenceBackend::cuda)) {
        return {};
    }

    try {
        const auto adapter = find_adapter(runtime.device.device_id);
        if (!adapter) return {};

        auto boost = std::make_unique<NvidiaLatencyBoost>(adapter.get());
        std::clog << "[SRV] GPU latency boost available: device=" << runtime.device.device_id
                  << " idle_timeout_ms=2000\n";
        return [boost = std::move(boost)](bool enabled) { return boost->set(enabled); };
    } catch (...) {
        std::clog << "[WARN] NVIDIA GPU latency boost unavailable\n";
        return {};
    }
}

} // namespace llavon::service
