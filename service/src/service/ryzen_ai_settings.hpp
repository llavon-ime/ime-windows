#pragma once
#include <ime-core/core.hpp>
#include <optional>

namespace llavon::service::ryzen_ai {
std::optional<ime::core::InferenceDeviceInfo> available_device();
std::filesystem::path onnx_model_directory(const std::filesystem::path& gguf_model);
std::shared_ptr<ime::core::InferenceAccelerator> create_accelerator(
    const std::filesystem::path& gguf_model, std::shared_ptr<ime::core::Logger> logger = {});
} // namespace llavon::service::ryzen_ai
