#include "settings_window.hpp"

#include "../resource.h"

#include <dwmapi.h>

#include <cstdint>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Input.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.h>

namespace llavon::settings {

struct SettingsWindow::UpdateNotificationTarget {
    std::mutex mutex;
    HWND window = nullptr;
};

namespace {

using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

constexpr wchar_t window_class_name[] = L"LlavonImeSettingsWindow";
constexpr UINT update_result_message = WM_APP + 10;
constexpr double section_title_size = 20;
constexpr double body_text_size = 14;
constexpr double caption_text_size = 12;
constexpr double control_height = 32;

SolidColorBrush solid_brush(std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return SolidColorBrush(winrt::Windows::UI::Color{255, red, green, blue});
}

TextBlock make_text(const wchar_t* value, double size, FontWeight weight = FontWeights::Normal()) {
    TextBlock block;
    block.Text(value);
    block.FontFamily(FontFamily(L"Segoe UI Variable Text, Microsoft JhengHei UI"));
    block.FontSize(size);
    block.FontWeight(weight);
    block.TextWrapping(TextWrapping::Wrap);
    return block;
}

TextBlock make_section_title(const wchar_t* value) {
    auto title = make_text(value, section_title_size, FontWeights::SemiBold());
    title.Margin(Thickness{0, 6, 0, 18});
    return title;
}

SolidColorBrush transparent_brush() {
    return SolidColorBrush(winrt::Windows::UI::Color{0, 0, 0, 0});
}

bool system_uses_dark_theme() {
    const auto foreground = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
        winrt::Windows::UI::ViewManagement::UIColorType::Foreground);
    return 5u * foreground.G + 2u * foreground.R + foreground.B > 8u * 128u;
}

std::wstring short_commit(std::wstring_view commit) {
    if (commit == L"unknown" || commit.size() < 7) {
        return L"開發版本";
    }
    return std::wstring(commit.substr(0, 7));
}

std::wstring build_identity(std::uint64_t build, std::wstring_view commit) {
    if (build == 0) {
        return L"開發版本（" + short_commit(commit) + L"）";
    }
    return L"建置 #" + std::to_wstring(build) + L"（" + short_commit(commit) + L"）";
}

const wchar_t* backend_label(std::int32_t backend) {
    switch (backend) {
        case LLAVON_SETTINGS_BACKEND_CUDA:
            return L"CUDA";
        case LLAVON_SETTINGS_BACKEND_VULKAN:
            return L"Vulkan";
        case LLAVON_SETTINGS_BACKEND_CPU:
            return L"CPU";
        default:
            return L"自動";
    }
}

std::wstring device_label(const InferenceDeviceOption& device) {
    std::wstring label = !device.description.empty() ? device.description : device.name;
    if (label.empty()) label = device.device_id;
    label += L"（";
    label += backend_label(device.backend);
    if (device.backend != LLAVON_SETTINGS_BACKEND_CPU && device.memory_total != 0) {
        std::wostringstream memory;
        memory << L"，" << std::fixed << std::setprecision(1)
               << static_cast<double>(device.memory_total) / 1024.0 / 1024.0 / 1024.0
               << L" GiB";
        label += memory.str();
    }
    label += L"）";
    return label;
}

}  // namespace

SettingsWindow::SettingsWindow(SettingsConfiguration configuration)
    : configuration_(std::move(configuration)) {}

SettingsWindow::~SettingsWindow() {
    destroy();
}

bool SettingsWindow::create(HINSTANCE instance) {
    if (window_) {
        return true;
    }

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon =
        LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_LLAVON_TRAY));
    if (!window_class.hIcon) {
        window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = window_class_name;
    window_class.hIconSm = window_class.hIcon;

    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    window_ = CreateWindowExW(0, window_class_name, L"Llavon 輸入法設定", WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 860, 640, nullptr, nullptr, instance, this);
    if (!window_) {
        return false;
    }

    update_target_ = std::make_shared<UpdateNotificationTarget>();
    update_target_->window = window_;

    try {
        initialize_xaml_island();
        build_page();
        update_theme();
    } catch (...) {
        destroy();
        throw;
    }
    return true;
}

void SettingsWindow::show() noexcept {
    if (!window_) {
        return;
    }
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    begin_update_check();
}

void SettingsWindow::hide() const noexcept {
    if (window_) {
        ShowWindow(window_, SW_HIDE);
    }
}

void SettingsWindow::destroy() noexcept {
    deactivate_update_target();
    discard_pending_update_results();
    close_xaml();
    if (window_) {
        const HWND window = window_;
        DestroyWindow(window);
        if (window_ == window) {
            window_ = nullptr;
        }
    }
}

bool SettingsWindow::pretranslate(MSG& message) const {
    if (!island_native_) {
        return false;
    }
    BOOL handled = FALSE;
    winrt::check_hresult(island_native_->PreTranslateMessage(&message, &handled));
    return handled != FALSE;
}

LRESULT CALLBACK SettingsWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    SettingsWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->handle_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SettingsWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == update_result_message) {
        std::unique_ptr<UpdateCheckResult> result(
            reinterpret_cast<UpdateCheckResult*>(lparam));
        if (result) {
            apply_update_result(std::move(*result));
        }
        return 0;
    }

    switch (message) {
        case WM_CLOSE:
            hide();
            return 0;
        case WM_SIZE:
            resize_island();
            return 0;
        case WM_DPICHANGED: {
            const auto suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left, suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            update_theme();
            return 0;
        case WM_DESTROY:
            close_xaml();
            return 0;
        case WM_NCDESTROY: {
            const HWND window = window_;
            deactivate_update_target();
            discard_pending_update_results();
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
            window_ = nullptr;
            return result;
        }
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
    }
}

void SettingsWindow::initialize_xaml_island() {
    xaml_manager_ = Hosting::WindowsXamlManager::InitializeForCurrentThread();
    xaml_source_ = Hosting::DesktopWindowXamlSource();
    island_native_ = xaml_source_.as<IDesktopWindowXamlSourceNative2>();
    winrt::check_hresult(island_native_->AttachToWindow(window_));
    winrt::check_hresult(island_native_->get_WindowHandle(&island_window_));
    resize_island();
}

void SettingsWindow::build_page() {
    shell_ = Grid();
    shell_.Background(transparent_brush());

    ScrollViewer scroll;
    scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);

    StackPanel page;
    page.Width(700);
    page.HorizontalAlignment(HorizontalAlignment::Left);
    page.Padding(Thickness{32, 24, 32, 40});

    page.Children().Append(make_section_title(L"推論裝置"));

    StackPanel inference_section;
    inference_section.Spacing(8);
    inference_section.Margin(Thickness{0, 0, 0, 24});

    Grid active_row;
    active_row.Width(540);
    active_row.HorizontalAlignment(HorizontalAlignment::Left);
    active_row.Background(transparent_brush());

    ColumnDefinition device_column;
    device_column.Width(GridLength{1, GridUnitType::Star});
    active_row.ColumnDefinitions().Append(device_column);
    ColumnDefinition status_column;
    status_column.Width(GridLength{1, GridUnitType::Auto});
    active_row.ColumnDefinitions().Append(status_column);

    const auto& active = configuration_.active_device;
    const std::wstring active_name =
        !active.description.empty() ? active.description
                                    : (!active.name.empty() ? active.name : active.device_id);
    active_device_status_ = make_text(
        active_name.c_str(), body_text_size, FontWeights::SemiBold());
    active_device_status_.TextTrimming(TextTrimming::CharacterEllipsis);
    active_device_status_.TextWrapping(TextWrapping::NoWrap);
    Grid::SetColumn(active_device_status_, 0);
    active_row.Children().Append(active_device_status_);

    std::wstring active_state = L"使用中 · ";
    active_state += backend_label(active.backend);
    TextBlock active_backend = make_text(active_state.c_str(), caption_text_size);
    active_backend.Margin(Thickness{16, 2, 0, 0});
    Grid::SetColumn(active_backend, 1);
    active_row.Children().Append(active_backend);

    ToolTip active_tooltip;
    StackPanel tooltip_content;
    tooltip_content.Spacing(4);
    tooltip_content.Children().Append(
        make_text(active_name.c_str(), body_text_size, FontWeights::SemiBold()));
    std::wstring backend_detail = L"後端：";
    backend_detail += backend_label(active.backend);
    tooltip_content.Children().Append(make_text(backend_detail.c_str(), caption_text_size));
    if (active.backend != LLAVON_SETTINGS_BACKEND_CPU && active.memory_total != 0) {
        std::wostringstream memory;
        memory << L"顯示記憶體：" << std::fixed << std::setprecision(1)
               << static_cast<double>(active.memory_total) / 1024.0 / 1024.0 / 1024.0
               << L" GB";
        tooltip_content.Children().Append(make_text(memory.str().c_str(), caption_text_size));
    }
    if (!active.device_id.empty()) {
        const std::wstring device_id = L"裝置 ID：" + active.device_id;
        tooltip_content.Children().Append(make_text(device_id.c_str(), caption_text_size));
    }
    const wchar_t* offload = configuration_.gpu_offload ? L"GPU offload：啟用"
                                                        : L"GPU offload：未啟用";
    tooltip_content.Children().Append(make_text(offload, caption_text_size));
    if (configuration_.fell_back_to_cpu) {
        tooltip_content.Children().Append(
            make_text(L"偏好裝置目前無法使用，已改用 CPU。", caption_text_size));
    }
    active_tooltip.Content(tooltip_content);
    ToolTipService::SetToolTip(active_row, active_tooltip);
    active_row.PointerEntered([active_tooltip](const auto&, const auto&) {
        active_tooltip.IsOpen(true);
    });
    active_row.PointerExited([active_tooltip](const auto&, const auto&) {
        active_tooltip.IsOpen(false);
    });
    inference_section.Children().Append(active_row);
    inference_section.Children().Append(make_text(L"下次啟動設定", body_text_size));

    inference_options_.clear();
    inference_options_.push_back(InferenceDeviceOption{
        .backend = LLAVON_SETTINGS_BACKEND_AUTO,
        .name = L"自動選擇",
    });
    inference_options_.push_back(InferenceDeviceOption{
        .backend = LLAVON_SETTINGS_BACKEND_CPU,
        .device_type = LLAVON_SETTINGS_DEVICE_CPU,
        .name = L"CPU",
    });
    for (const auto& device : configuration_.devices) {
        if (device.backend == LLAVON_SETTINGS_BACKEND_CUDA ||
            device.backend == LLAVON_SETTINGS_BACKEND_VULKAN) {
            inference_options_.push_back(device);
        }
    }

    bool selected_device_available =
        configuration_.selected_backend == LLAVON_SETTINGS_BACKEND_AUTO ||
        configuration_.selected_backend == LLAVON_SETTINGS_BACKEND_CPU;
    for (const auto& option : inference_options_) {
        if (option.backend == configuration_.selected_backend &&
            option.device_id == configuration_.selected_device_id) {
            selected_device_available = true;
            break;
        }
    }
    if (!selected_device_available) {
        inference_options_.push_back(InferenceDeviceOption{
            .backend = configuration_.selected_backend,
            .device_id = configuration_.selected_device_id,
            .name = std::wstring(L"原設定：") + backend_label(configuration_.selected_backend) +
                    L" / " + configuration_.selected_device_id + L"（目前不可用）",
        });
    }

    inference_device_ = ComboBox();
    inference_device_.Width(540);
    inference_device_.MinHeight(control_height);
    inference_device_.FontSize(body_text_size);
    inference_device_.HorizontalAlignment(HorizontalAlignment::Left);
    inference_device_.Items().Append(winrt::box_value(L"自動選擇（建議）"));
    inference_device_.Items().Append(winrt::box_value(L"CPU"));
    for (std::size_t index = 2; index < inference_options_.size(); ++index) {
        const auto& option = inference_options_[index];
        const bool unavailable = !selected_device_available &&
                                 option.backend == configuration_.selected_backend &&
                                 option.device_id == configuration_.selected_device_id;
        inference_device_.Items().Append(
            winrt::box_value(unavailable ? option.name : device_label(option)));
    }

    std::int32_t selected_index = 0;
    for (std::size_t index = 0; index < inference_options_.size(); ++index) {
        const auto& option = inference_options_[index];
        if (option.backend == configuration_.selected_backend &&
            option.device_id == configuration_.selected_device_id) {
            selected_index = static_cast<std::int32_t>(index);
            break;
        }
    }
    inference_device_.SelectedIndex(selected_index);
    inference_device_.SelectionChanged(
        [this](const auto&, const auto&) { update_inference_save_state(); });
    inference_section.Children().Append(inference_device_);

    save_inference_button_ = Button();
    save_inference_button_.Content(winrt::box_value(L"儲存裝置設定"));
    save_inference_button_.MinWidth(132);
    save_inference_button_.MinHeight(control_height);
    save_inference_button_.FontSize(body_text_size);
    save_inference_button_.HorizontalAlignment(HorizontalAlignment::Left);
    save_inference_button_.IsEnabled(false);
    save_inference_button_.Click([this](const auto&, const auto&) { save_inference_setting(); });
    inference_section.Children().Append(save_inference_button_);

    note_ = make_text(L"", caption_text_size);
    note_.Visibility(Visibility::Collapsed);
    inference_section.Children().Append(note_);
    page.Children().Append(inference_section);

    page.Children().Append(make_section_title(L"軟體更新"));

    StackPanel update_section;
    update_section.Spacing(8);
    update_section.Margin(Thickness{0, 0, 0, 24});

    const std::wstring build_label =
        L"目前：" + build_identity(UpdateChecker::installed_build_number(),
                                    UpdateChecker::installed_commit());
    update_section.Children().Append(make_text(build_label.c_str(), body_text_size));

    StackPanel update_row;
    update_row.Orientation(Orientation::Horizontal);
    update_row.Spacing(12);

    update_button_ = Button();
    update_button_.Content(winrt::box_value(L"檢查更新"));
    update_button_.MinWidth(112);
    update_button_.MinHeight(control_height);
    update_button_.FontSize(body_text_size);
    update_button_.Click([this](const auto&, const auto&) { begin_update_check(); });
    update_row.Children().Append(update_button_);

    update_download_ = HyperlinkButton();
    update_download_.Visibility(Visibility::Collapsed);
    update_download_.MinHeight(control_height);
    update_download_.FontSize(body_text_size);
    update_row.Children().Append(update_download_);
    update_section.Children().Append(update_row);

    update_status_ = make_text(L"開啟設定時會自動與 latest 建置比較。", caption_text_size);
    update_section.Children().Append(update_status_);
    page.Children().Append(update_section);

    scroll.Content(page);
    shell_.Children().Append(scroll);
    xaml_source_.Content(shell_);
}

void SettingsWindow::save_inference_setting() {
    if (!inference_device_ || !note_ || !configuration_.save_callback) {
        return;
    }
    const std::int32_t selected_index = inference_device_.SelectedIndex();
    if (selected_index < 0 ||
        static_cast<std::size_t>(selected_index) >= inference_options_.size()) {
        note_.Text(L"請先選擇推論裝置。");
        return;
    }

    const auto& option = inference_options_[static_cast<std::size_t>(selected_index)];
    const std::int32_t result = configuration_.save_callback(
        configuration_.save_context, option.backend, option.device_id.c_str());
    if (result == ERROR_SUCCESS) {
        configuration_.selected_backend = option.backend;
        configuration_.selected_device_id = option.device_id;
        save_inference_button_.IsEnabled(false);
        note_.Text(L"已儲存");
        note_.Visibility(Visibility::Visible);
    } else {
        note_.Text(L"無法儲存推論裝置設定，請稍後再試。");
        note_.Visibility(Visibility::Visible);
    }
}

void SettingsWindow::update_inference_save_state() {
    if (!inference_device_ || !save_inference_button_ || !note_) return;
    const std::int32_t selected_index = inference_device_.SelectedIndex();
    if (selected_index < 0 ||
        static_cast<std::size_t>(selected_index) >= inference_options_.size()) {
        save_inference_button_.IsEnabled(false);
        note_.Visibility(Visibility::Collapsed);
        return;
    }

    const auto& option = inference_options_[static_cast<std::size_t>(selected_index)];
    const bool changed = option.backend != configuration_.selected_backend ||
                         option.device_id != configuration_.selected_device_id;
    save_inference_button_.IsEnabled(changed);
    if (changed) {
        note_.Text(L"重新啟動後套用");
        note_.Visibility(Visibility::Visible);
    } else {
        note_.Visibility(Visibility::Collapsed);
    }
}

void SettingsWindow::begin_update_check() {
    if (!update_button_ || !update_status_ || !update_target_) {
        return;
    }

    update_button_.IsEnabled(false);
    update_status_.Text(L"正在檢查 latest 建置…");
    set_update_status_tone(UpdateStatusTone::secondary);
    if (update_download_) {
        update_download_.Visibility(Visibility::Collapsed);
    }

    const auto target = update_target_;
    if (!update_checker_.check_async([target](UpdateCheckResult result) {
            auto pending = std::make_unique<UpdateCheckResult>(std::move(result));
            std::lock_guard lock(target->mutex);
            if (target->window &&
                PostMessageW(target->window, update_result_message, 0,
                             reinterpret_cast<LPARAM>(pending.get()))) {
                pending.release();
            }
        })) {
        update_status_.Text(L"更新檢查已在進行中。");
    }
}

void SettingsWindow::apply_update_result(UpdateCheckResult result) {
    if (!update_button_ || !update_status_ || !update_download_) {
        return;
    }

    update_button_.IsEnabled(true);
    switch (result.status) {
        case UpdateCheckStatus::update_available: {
            const std::wstring status =
                L"有新建置：#" + std::to_wstring(result.current_build) + L" → #" +
                std::to_wstring(result.latest_build) + L"。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::update_available);

            const std::wstring download =
                L"下載 latest（#" + std::to_wstring(result.latest_build) + L"）";
            update_download_.Content(winrt::box_value(download));
            update_download_.NavigateUri(winrt::Windows::Foundation::Uri(result.release_url));
            update_download_.Visibility(Visibility::Visible);
            break;
        }
        case UpdateCheckStatus::up_to_date: {
            const std::wstring status =
                L"已是最新建置：" + build_identity(result.current_build, result.current_commit) + L"。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::success);
            update_download_.Visibility(Visibility::Collapsed);
            break;
        }
        case UpdateCheckStatus::local_newer: {
            const std::wstring status =
                L"目前建置 #" + std::to_wstring(result.current_build) +
                L" 比 latest #" + std::to_wstring(result.latest_build) + L" 新。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::information);
            update_download_.Visibility(Visibility::Collapsed);
            break;
        }
        case UpdateCheckStatus::development_build: {
            const std::wstring status =
                build_identity(result.current_build, result.current_commit) + L" 與 latest #" +
                std::to_wstring(result.latest_build) + L"（" + short_commit(result.latest_commit) +
                L"）不同，無法判斷新舊。";
            update_status_.Text(status);
            set_update_status_tone(UpdateStatusTone::secondary);
            const std::wstring download =
                L"下載 latest（#" + std::to_wstring(result.latest_build) + L"）";
            update_download_.Content(winrt::box_value(download));
            update_download_.NavigateUri(winrt::Windows::Foundation::Uri(result.release_url));
            update_download_.Visibility(Visibility::Visible);
            break;
        }
        case UpdateCheckStatus::failed:
        default:
            update_status_.Text(L"無法檢查更新：" + result.error_message);
            set_update_status_tone(UpdateStatusTone::error);
            update_download_.Visibility(Visibility::Collapsed);
            break;
    }
}

void SettingsWindow::deactivate_update_target() noexcept {
    if (!update_target_) {
        return;
    }
    std::lock_guard lock(update_target_->mutex);
    update_target_->window = nullptr;
}

void SettingsWindow::discard_pending_update_results() noexcept {
    if (!window_) {
        return;
    }
    MSG message{};
    while (PeekMessageW(&message, window_, update_result_message, update_result_message, PM_REMOVE)) {
        delete reinterpret_cast<UpdateCheckResult*>(message.lParam);
    }
}

void SettingsWindow::resize_island() const noexcept {
    if (!window_ || !island_window_) {
        return;
    }
    RECT client{};
    GetClientRect(window_, &client);
    SetWindowPos(island_window_, nullptr, 0, 0, client.right - client.left, client.bottom - client.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

void SettingsWindow::update_theme() {
    if (!window_) {
        return;
    }

    try {
        dark_theme_ = system_uses_dark_theme();
    } catch (...) {
        // Keep the last successfully detected theme if UISettings is temporarily unavailable.
    }

    const BOOL dark = dark_theme_ ? TRUE : FALSE;
    DwmSetWindowAttribute(window_, 20, &dark, sizeof(dark));

    const DWM_SYSTEMBACKDROP_TYPE backdrop = DWMSBT_MAINWINDOW;
    const HRESULT backdrop_result = DwmSetWindowAttribute(
        window_, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

    if (shell_) {
        shell_.RequestedTheme(dark_theme_ ? ElementTheme::Dark : ElementTheme::Light);
        shell_.Background(
            SUCCEEDED(backdrop_result)
                ? transparent_brush()
                : (dark_theme_ ? solid_brush(32, 32, 32) : solid_brush(248, 244, 235)));
    }
    apply_theme_colors();
}

void SettingsWindow::apply_theme_colors() {
    if (note_) {
        note_.Foreground(dark_theme_ ? solid_brush(190, 190, 190) : solid_brush(96, 96, 96));
    }
    if (update_download_) {
        update_download_.Foreground(
            dark_theme_ ? solid_brush(255, 153, 164) : solid_brush(210, 36, 36));
    }
    set_update_status_tone(update_status_tone_);
}

void SettingsWindow::set_update_status_tone(UpdateStatusTone tone) {
    update_status_tone_ = tone;
    if (!update_status_) {
        return;
    }

    switch (tone) {
        case UpdateStatusTone::update_available:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(255, 153, 164) : solid_brush(210, 36, 36));
            break;
        case UpdateStatusTone::success:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(108, 203, 95) : solid_brush(48, 120, 72));
            break;
        case UpdateStatusTone::information:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(117, 182, 231) : solid_brush(48, 96, 156));
            break;
        case UpdateStatusTone::error:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(255, 153, 164) : solid_brush(180, 54, 54));
            break;
        case UpdateStatusTone::secondary:
        default:
            update_status_.Foreground(
                dark_theme_ ? solid_brush(190, 190, 190) : solid_brush(96, 96, 96));
            break;
    }
}

void SettingsWindow::close_xaml() noexcept {
    island_window_ = nullptr;
    island_native_ = nullptr;
    active_device_status_ = nullptr;
    inference_device_ = nullptr;
    save_inference_button_ = nullptr;
    update_button_ = nullptr;
    update_status_ = nullptr;
    update_download_ = nullptr;
    note_ = nullptr;
    shell_ = nullptr;
    try {
        if (xaml_source_) {
            xaml_source_.Close();
            xaml_source_ = nullptr;
        }
        if (xaml_manager_) {
            xaml_manager_.Close();
            xaml_manager_ = nullptr;
        }
    } catch (...) {
    }
}

}  // namespace llavon::settings
