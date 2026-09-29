#include "ryzen_ai_settings.hpp"
#include "npu_compiler_process.hpp"
#include <ime-core/ryzen_ai.hpp>
#include <ime-core/logger.hpp>

#include <Windows.h>
#include <ShlObj.h>
#include <SetupAPI.h>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace llavon::service::ryzen_ai {
std::optional<ime::core::InferenceDeviceInfo> available_device() {
    const auto devices = SetupDiGetClassDevsW(nullptr, L"PCI", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (devices == INVALID_HANDLE_VALUE) return std::nullopt;
    struct DeviceSet {
        HDEVINFO value;
        ~DeviceSet() { SetupDiDestroyDeviceInfoList(value); }
    } owned{devices};
    bool found = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA device{};
        device.cbSize = sizeof(device);
        if (!SetupDiEnumDeviceInfo(devices, index, &device)) break;
        std::array<wchar_t, 2048> ids{};
        if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, SPDRP_HARDWAREID,
            nullptr, reinterpret_cast<PBYTE>(ids.data()), static_cast<DWORD>(sizeof(ids)), nullptr)) continue;
        // AMD documents 1022:17F0 for STX/KRK (XDNA 2). This is a read-only
        // device property query, including when the device driver is missing.
        const std::wstring_view id(ids.data());
        if (id.find(L"VEN_1022&DEV_17F0") != std::wstring_view::npos) { found = true; break; }
    }
    if (!found) return std::nullopt;
    return ime::core::InferenceDeviceInfo{.backend = ime::core::InferenceBackend::ryzen_ai,
        .type = ime::core::InferenceDeviceType::npu, .device_id = "RYZENAI-NPU",
        .name = "AMD Ryzen AI NPU", .description = "Windows ML / VitisAI INT4 ONNX"};
}

namespace {
std::optional<std::filesystem::path> environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return std::nullopt;
    std::wstring value(required, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), required);
    if (copied == 0 || copied >= required) return std::nullopt;
    value.resize(copied);
    return std::filesystem::path(std::move(value));
}

std::filesystem::path local_app_data() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw))) {
        throw std::runtime_error("Cannot locate LocalAppData for the ONNX NPU cache");
    }
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    return owned.get();
}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) throw std::runtime_error("Cannot locate the service executable");
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool is_genai_model(const std::filesystem::path& directory) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / L"genai_config.json", error) &&
           std::filesystem::is_regular_file(directory / L"model.onnx", error) &&
           std::filesystem::is_regular_file(directory / L"model.onnx.data", error);
}

class IsolatedCompilerAccelerator final : public ime::core::InferenceAccelerator {
public:
    IsolatedCompilerAccelerator(ime::core::RyzenAiConfig config,
                                std::shared_ptr<ime::core::Logger> logger)
        : config_(std::move(config)), logger_(std::move(logger)) {
        config_.allow_compilation = false;
        accelerator_ = ime::core::create_ryzen_ai_accelerator(config_);
    }

    void prepare(const std::filesystem::path& model, std::uint32_t context_length) override {
        const std::array arguments{config_.model_directory.wstring(), config_.cache_directory.wstring(),
                                   std::to_wstring(context_length)};
        if (logger_) logger_->log("[NPU] preparing ONNX context in compiler process");
        detail::run_compiler_process(detail::compiler_executable(), arguments,
            [this](std::string message) {
                if (logger_) logger_->log("[NPU compiler] " + message);
            });
        // Only load the checked cache here. Never compile or retry compilation
        // inside the IME service, even if the context is missing or stale.
        accelerator_->prepare(model, context_length);
    }

    std::unique_ptr<ime::core::InferenceContext> create_context() override {
        return accelerator_->create_context();
    }

    ime::core::InferenceDeviceInfo device_info() const override {
        return accelerator_->device_info();
    }

private:
    ime::core::RyzenAiConfig config_;
    std::shared_ptr<ime::core::Logger> logger_;
    std::shared_ptr<ime::core::InferenceAccelerator> accelerator_;
};
} // namespace

std::filesystem::path onnx_model_directory(const std::filesystem::path& gguf_model) {
    if (const auto override = environment_path(L"LLAVON_IME_NPU_MODEL_PATH")) {
        return override->is_absolute() ? override->lexically_normal()
                                       : std::filesystem::absolute(*override).lexically_normal();
    }
    const auto name = gguf_model.stem().wstring() + L"-onnx";
    const auto beside_gguf = gguf_model.parent_path() / name;
    if (is_genai_model(beside_gguf)) return beside_gguf;

    const auto installed = executable_directory().parent_path() / L"models" / name;
    if (is_genai_model(installed)) return installed;
    return beside_gguf;
}

std::shared_ptr<ime::core::InferenceAccelerator> create_accelerator(
    const std::filesystem::path& gguf_model, std::shared_ptr<ime::core::Logger> logger) {
    if (!available_device()) throw std::runtime_error("No AMD XDNA 2 NPU (PCI 1022:17F0) is present");
    ime::core::RyzenAiConfig config{
        .model_directory = onnx_model_directory(gguf_model),
        .cache_directory = local_app_data() / L"Llavon IME" / L"onnx-npu-cache",
        .logger = logger,
    };
    if (logger) {
        logger->log(ime::core::LogInformation::general,
                    "[NPU] ONNX model: " + config.model_directory.string());
        logger->log(ime::core::LogInformation::general,
                    "[NPU] compiled cache: " + config.cache_directory.string());
    }
    return std::make_shared<IsolatedCompilerAccelerator>(std::move(config), std::move(logger));
}
} // namespace llavon::service::ryzen_ai
