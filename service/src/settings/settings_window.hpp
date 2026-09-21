#pragma once

#include "settings_configuration.hpp"
#include "update_checker.hpp"

#include <windows.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/base.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llavon::settings {

class SettingsWindow final {
public:
    explicit SettingsWindow(SettingsConfiguration configuration);
    SettingsWindow(const SettingsWindow&) = delete;
    SettingsWindow& operator=(const SettingsWindow&) = delete;
    ~SettingsWindow();

    bool create(HINSTANCE instance);
    void show() noexcept;
    void hide() const noexcept;
    void destroy() noexcept;
    bool pretranslate(MSG& message) const;

private:
    struct CustomNameEntry;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    void initialize_xaml_island();
    void build_page();
    void browse_model_file();
    void save_model_path();
    void update_model_path_save_state();
    void save_inference_setting();
    void update_inference_save_state();
    void add_custom_name_row(
        std::u16string name = {}, std::vector<std::u16string> readings = {});
    void remove_custom_name_row(
        const winrt::Windows::UI::Xaml::Controls::Button& remove_button);
    void refresh_custom_name_pronunciations(
        const winrt::Windows::UI::Xaml::Controls::TextBox& name_box);
    void save_custom_names();
    void update_custom_names_save_state();
    bool collect_custom_names(std::vector<CustomNameEntry>& entries) const;
    const std::vector<std::u16string>& lookup_bopomofo(char32_t character) const;
    void begin_update_check();
    void apply_update_result(UpdateCheckResult result);
    void deactivate_update_target() noexcept;
    void discard_pending_update_results() noexcept;
    void resize_island() const noexcept;
    void update_theme();
    void apply_theme_colors();
    void close_xaml() noexcept;

    enum class UpdateStatusTone {
        secondary,
        update_available,
        success,
        information,
        error,
    };

    void set_update_status_tone(UpdateStatusTone tone);

    HWND window_ = nullptr;
    HWND island_window_ = nullptr;
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager xaml_manager_{nullptr};
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{nullptr};
    winrt::com_ptr<IDesktopWindowXamlSourceNative2> island_native_;
    winrt::Windows::UI::Xaml::Controls::Grid shell_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBox model_path_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button browse_model_button_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button save_model_button_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock model_note_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock active_device_status_{nullptr};
    winrt::Windows::UI::Xaml::Controls::ComboBox inference_device_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button save_inference_button_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button update_button_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock update_status_{nullptr};
    winrt::Windows::UI::Xaml::Controls::HyperlinkButton update_download_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock note_{nullptr};
    winrt::Windows::UI::Xaml::Controls::StackPanel custom_names_panel_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button add_custom_name_button_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button save_custom_names_button_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock custom_names_note_{nullptr};
    UpdateStatusTone update_status_tone_ = UpdateStatusTone::secondary;
    bool dark_theme_ = false;
    SettingsConfiguration configuration_;
    std::vector<InferenceDeviceOption> inference_options_;

    struct CustomNameEntry {
        std::u16string name;
        std::vector<std::u16string> readings;

        bool operator==(const CustomNameEntry&) const = default;
    };

    struct CustomNameRow {
        winrt::Windows::UI::Xaml::Controls::Grid container{nullptr};
        winrt::Windows::UI::Xaml::Controls::TextBox name{nullptr};
        winrt::Windows::UI::Xaml::Controls::StackPanel pronunciations{nullptr};
        winrt::Windows::UI::Xaml::Controls::Button remove_button{nullptr};
        std::vector<winrt::Windows::UI::Xaml::Controls::ComboBox> reading_choices;
        bool missing_pronunciation = false;
    };

    std::vector<CustomNameRow> custom_name_rows_;
    std::vector<CustomNameEntry> saved_custom_names_;

    struct UpdateNotificationTarget;
    std::shared_ptr<UpdateNotificationTarget> update_target_;
    UpdateChecker update_checker_;
};

}  // namespace llavon::settings
