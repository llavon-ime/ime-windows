#include "user_settings_codec.hpp"

#include <rfl/DefaultIfMissing.hpp>
#include <rfl/json.hpp>
#include <utf8/cpp20.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace llavon::service::user_settings_codec {
namespace {

using llavon::ime::core::InferenceBackend;
using llavon::ime::core::InferenceDeviceSelection;
constexpr std::uint32_t current_schema = 1;

struct InferenceSettingsDocument {
    std::string backend = "auto";
    std::string device_id;
};

struct CustomNameDocument {
    std::string name;
    std::vector<std::string> readings;
};

struct SettingsDocument {
    std::uint32_t schema = current_schema;
    InferenceSettingsDocument inference;
    std::string model_path;
    std::vector<CustomNameDocument> custom_names;
    bool shift_space_width_toggle_enabled = false;
    bool major_update_notifications_enabled = true;
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

std::optional<UserSettings> decode(std::string_view json) {
    auto parsed = rfl::json::read<SettingsDocument, rfl::DefaultIfMissing>(json);
    if (!parsed) return std::nullopt;

    SettingsDocument document = std::move(parsed).value();
    if (document.schema != current_schema) return std::nullopt;
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
    std::vector<CustomNameSetting> custom_names;
    custom_names.reserve(document.custom_names.size());
    try {
        for (const auto& entry : document.custom_names) {
            if (entry.name.empty() || entry.readings.empty()) return std::nullopt;

            auto name = utf8::utf8to32(entry.name);
            std::vector<std::u16string> readings;
            readings.reserve(entry.readings.size());
            for (const auto& reading : entry.readings) {
                if (reading.empty()) return std::nullopt;
                readings.push_back(utf8::utf8to16(reading));
            }
            if (name.size() != readings.size()) return std::nullopt;
            custom_names.push_back(CustomNameSetting{
                .name = std::move(name),
                .readings = std::move(readings),
            });
        }
    } catch (const utf8::exception&) {
        return std::nullopt;
    }
    return UserSettings{
        .inference = std::move(selection),
        .model_path = std::move(document.model_path),
        .custom_names = std::move(custom_names),
        .shift_space_width_toggle_enabled =
            document.shift_space_width_toggle_enabled,
        .major_update_notifications_enabled =
            document.major_update_notifications_enabled,
    };
}

std::string encode(const UserSettings& settings) {
    const auto& selection = settings.inference;
    const bool device_is_relevant = selection.backend == InferenceBackend::cuda ||
                                    selection.backend == InferenceBackend::vulkan;
    std::vector<CustomNameDocument> custom_names;
    custom_names.reserve(settings.custom_names.size());
    for (const auto& entry : settings.custom_names) {
        if (entry.name.empty() || entry.name.size() != entry.readings.size()) {
            throw std::invalid_argument("custom name and reading counts must match");
        }
        std::vector<std::string> readings;
        readings.reserve(entry.readings.size());
        for (const auto& reading : entry.readings) {
            if (reading.empty()) {
                throw std::invalid_argument("custom name readings must not be empty");
            }
            readings.push_back(utf8::utf16to8(reading));
        }
        custom_names.push_back(CustomNameDocument{
            .name = utf8::utf32to8(entry.name),
            .readings = std::move(readings),
        });
    }
    const SettingsDocument document{
        .schema = current_schema,
        .inference = InferenceSettingsDocument{
            .backend = backend_name(selection.backend),
            .device_id = device_is_relevant ? selection.device_id : std::string{},
        },
        .model_path = settings.model_path,
        .custom_names = std::move(custom_names),
        .shift_space_width_toggle_enabled =
            settings.shift_space_width_toggle_enabled,
        .major_update_notifications_enabled =
            settings.major_update_notifications_enabled,
    };
    return rfl::json::write(document, YYJSON_WRITE_PRETTY);
}

}  // namespace llavon::service::user_settings_codec
