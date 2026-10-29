#include "service/user_settings_codec.hpp"

#include <cstdio>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main() {
    try {
        using namespace llavon::service;
        UserSettings settings;
        settings.immediate_conversion_enabled = true;
        settings.shift_space_width_toggle_enabled = true;
        settings.gpu_boost_enabled = false;
        settings.model_path = "model.gguf";
        settings.custom_names = {{U"小明", {u"ㄒㄧㄠˇ", u"ㄇㄧㄥˊ"}}};
        auto restored = user_settings_codec::decode(user_settings_codec::encode(settings));
        require(restored && restored->immediate_conversion_enabled,
                "enabled prediction display setting did not round trip");
        require(restored->shift_space_width_toggle_enabled && !restored->gpu_boost_enabled &&
                    restored->model_path == settings.model_path &&
                    restored->custom_names == settings.custom_names,
                "prediction display setting changed unrelated settings");
        restored->immediate_conversion_enabled = false;
        const auto disabled = user_settings_codec::decode(user_settings_codec::encode(*restored));
        require(disabled && !disabled->immediate_conversion_enabled,
                "disabled prediction display setting did not round trip");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
