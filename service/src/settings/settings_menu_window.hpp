#pragma once

#include <windows.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/base.h>

#include <functional>

namespace llavon::settings {

class SettingsMenuWindow final {
public:
    explicit SettingsMenuWindow(std::function<void()> open_settings);
    SettingsMenuWindow(const SettingsMenuWindow&) = delete;
    SettingsMenuWindow& operator=(const SettingsMenuWindow&) = delete;
    ~SettingsMenuWindow();

    bool show(HINSTANCE instance, POINT anchor);
    void hide() const noexcept;
    void destroy() noexcept;
    bool pretranslate(MSG& message) const;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam,
                                        LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool create(HINSTANCE instance);
    void initialize_xaml_island();
    void build_content();
    void resize_island() const noexcept;
    void apply_round_region(int width, int height) const noexcept;
    void close_xaml() noexcept;

    HWND window_ = nullptr;
    HWND island_window_ = nullptr;
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager xaml_manager_{nullptr};
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{nullptr};
    winrt::com_ptr<IDesktopWindowXamlSourceNative2> island_native_;
    winrt::Windows::UI::Xaml::Controls::Button settings_button_{nullptr};
    std::function<void()> open_settings_;
};

}  // namespace llavon::settings
