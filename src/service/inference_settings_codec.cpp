#include "inference_settings_codec.hpp"

#include <rfl/DefaultIfMissing.hpp>
#include <rfl/json.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace llavon::service::inference_settings_codec {
namespace {

using llavon::ime::core::InferenceBackend;
using llavon::ime::core::InferenceDeviceSelection;

struct InferenceSettingsDocument {
    std::string backend = "auto";
    std::string device_id;
};

struct SettingsDocument {
    std::uint32_t schema = 1;
    InferenceSettingsDocument inference;
};

const char* backend_name(InferenceBackend backend) {
    switch (backend) {
        case InferenceBackend::automatic:
            return "auto";
        case InferenceBackend::cpu:
            return "cpu";
        case InferenceBackend::cuda:
            return "cuda";
        case InferenceBackend::vulkan:
            return "vulkan";
        default:
            return "auto";
    }
}

std::optional<InferenceBackend> parse_backend(const std::string& value) {
    if (value == "auto") return InferenceBackend::automatic;
    if (value == "cpu") return InferenceBackend::cpu;
    if (value == "cuda") return InferenceBackend::cuda;
    if (value == "vulkan") return InferenceBackend::vulkan;
    return std::nullopt;
}

}  // namespace

std::optional<InferenceDeviceSelection> decode(std::string_view json) {
    auto parsed = rfl::json::read<SettingsDocument, rfl::DefaultIfMissing>(json);
    if (!parsed) return std::nullopt;

    SettingsDocument document = std::move(parsed).value();
    if (document.schema != 1) return std::nullopt;
    const auto backend = parse_backend(document.inference.backend);
    if (!backend) return std::nullopt;

    InferenceDeviceSelection selection{
        .backend = *backend,
        .device_id = std::move(document.inference.device_id),
    };
    if (selection.backend == InferenceBackend::automatic ||
        selection.backend == InferenceBackend::cpu) {
        selection.device_id.clear();
    }
    return selection;
}

std::string encode(const InferenceDeviceSelection& selection) {
    const bool device_is_relevant = selection.backend == InferenceBackend::cuda ||
                                    selection.backend == InferenceBackend::vulkan;
    const SettingsDocument document{
        .schema = 1,
        .inference = InferenceSettingsDocument{
            .backend = backend_name(selection.backend),
            .device_id = device_is_relevant ? selection.device_id : std::string{},
        },
    };
    return rfl::json::write(document, YYJSON_WRITE_PRETTY);
}

}  // namespace llavon::service::inference_settings_codec
