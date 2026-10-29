#pragma once

#include "settings_configuration.hpp"
#include "../ui/ui_thread.hpp"

#include <windows.h>

#include <memory>

namespace llavon::settings {

class SettingsUi final {
public:
    SettingsUi(ui::UiThread& thread, SettingsConfiguration configuration);
    ~SettingsUi();
    SettingsUi(const SettingsUi&) = delete;
    SettingsUi& operator=(const SettingsUi&) = delete;

    void show() const noexcept;
    void hide() const noexcept;
    void show_context_menu(POINT location) const noexcept;
    void notify_pending_count(std::size_t count) noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace llavon::settings
