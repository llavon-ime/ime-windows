#include "settings_menu_window.hpp"

#include <dwmapi.h>

#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.h>

#include <algorithm>
#include <cstdint>
#include <utility>

namespace llavon::settings {
namespace {

constexpr wchar_t menu_window_class[] = L"LlavonImeSettingsMenuWindow";
constexpr int menu_width_dip = 220;
constexpr int menu_height_dip = 52;
constexpr int menu_gap_dip = 6;
constexpr int menu_corner_radius_dip = 8;

using winrt::Windows::UI::Color;
using winrt::Windows::UI::Xaml::Media::SolidColorBrush;

Color color(std::uint8_t red, std::uint8_t green, std::uint8_t blue,
            std::uint8_t alpha = 255) {
    return Color{alpha, red, green, blue};
}

SolidColorBrush brush(Color value) {
    return SolidColorBrush(value);
}

bool system_uses_dark_theme() noexcept {
    try {
        const Color background = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
            winrt::Windows::UI::ViewManagement::UIColorType::Background);
        const unsigned int luminance =
            2126u * background.R + 7152u * background.G + 722u * background.B;
        return luminance < 128u * 10000u;
    } catch (...) {
        return false;
    }
}

bool font_available(const wchar_t* face_name) noexcept {
    HDC dc = GetDC(nullptr);
    if (!dc) return false;

    LOGFONTW font{};
    font.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(font.lfFaceName, face_name);
    bool found = false;
    EnumFontFamiliesExW(
        dc, &font,
        [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM context) -> int {
            *reinterpret_cast<bool*>(context) = true;
            return 0;
        },
        reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    return found;
}

int scale(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

}  // namespace

SettingsMenuWindow::SettingsMenuWindow(std::function<void()> open_settings)
    : open_settings_(std::move(open_settings)) {}

SettingsMenuWindow::~SettingsMenuWindow() {
    destroy();
}

bool SettingsMenuWindow::show(HINSTANCE instance, POINT anchor) {
    if (!create(instance)) {
        return false;
    }
    if (anchor.x < 0 || anchor.y < 0) {
        GetCursorPos(&anchor);
    }

    const UINT dpi = GetDpiForWindow(window_);
    const int width = scale(menu_width_dip, dpi);
    const int height = scale(menu_height_dip, dpi);
    const int gap = scale(menu_gap_dip, dpi);

    MONITORINFO monitor_info{sizeof(monitor_info)};
    const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    GetMonitorInfoW(monitor, &monitor_info);
    const RECT work = monitor_info.rcWork;

    int x = anchor.x - width / 2;
    int y = anchor.y - height - gap;
    if (anchor.y >= work.bottom) {
        y = work.bottom - height - gap;
    } else if (anchor.y <= work.top || y < work.top) {
        y = std::max(work.top + gap, anchor.y + gap);
    }
    x = std::clamp(x, static_cast<int>(work.left),
                   std::max(static_cast<int>(work.left), static_cast<int>(work.right) - width));
    y = std::clamp(y, static_cast<int>(work.top),
                   std::max(static_cast<int>(work.top), static_cast<int>(work.bottom) - height));

    apply_round_region(width, height);
    SetWindowPos(window_, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
    ShowWindow(window_, SW_SHOWNORMAL);
    SetForegroundWindow(window_);
    return true;
}

void SettingsMenuWindow::hide() const noexcept {
    if (window_) {
        ShowWindow(window_, SW_HIDE);
    }
}

void SettingsMenuWindow::destroy() noexcept {
    close_xaml();
    if (window_) {
        const HWND window = window_;
        DestroyWindow(window);
        if (window_ == window) window_ = nullptr;
    }
}

bool SettingsMenuWindow::pretranslate(MSG& message) const {
    if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE && window_ &&
        IsWindowVisible(window_)) {
        hide();
        return true;
    }
    if (!island_native_) return false;

    BOOL handled = FALSE;
    winrt::check_hresult(island_native_->PreTranslateMessage(&message, &handled));
    return handled != FALSE;
}

LRESULT CALLBACK SettingsMenuWindow::window_proc(HWND window, UINT message, WPARAM wparam,
                                                  LPARAM lparam) {
    SettingsMenuWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsMenuWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<SettingsMenuWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->handle_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT SettingsMenuWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) hide();
            return 0;
        case WM_CLOSE:
            hide();
            return 0;
        case WM_SIZE:
            resize_island();
            return 0;
        case WM_DPICHANGED: {
            const auto suggested = reinterpret_cast<RECT*>(lparam);
            const int width = suggested->right - suggested->left;
            const int height = suggested->bottom - suggested->top;
            apply_round_region(width, height);
            SetWindowPos(window_, HWND_TOPMOST, suggested->left, suggested->top, width, height,
                         SWP_NOACTIVATE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED:
        case WM_SYSCOLORCHANGE:
            if (xaml_source_) build_content();
            return 0;
        case WM_DESTROY:
            close_xaml();
            return 0;
        case WM_NCDESTROY: {
            const HWND window = window_;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            const LRESULT result = DefWindowProcW(window, message, wparam, lparam);
            window_ = nullptr;
            return result;
        }
        default:
            return DefWindowProcW(window_, message, wparam, lparam);
    }
}

bool SettingsMenuWindow::create(HINSTANCE instance) {
    if (window_) return true;

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_DROPSHADOW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    window_class.lpszClassName = menu_window_class;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, menu_window_class,
                              L"拉風輸入法", WS_POPUP, 0, 0, menu_width_dip,
                              menu_height_dip, nullptr, nullptr, instance, this);
    if (!window_) return false;

    try {
        initialize_xaml_island();
        build_content();

        const DWM_WINDOW_CORNER_PREFERENCE preference = DWMWCP_ROUND;
        DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                              sizeof(preference));
        return true;
    } catch (...) {
        destroy();
        return false;
    }
}

void SettingsMenuWindow::initialize_xaml_island() {
    xaml_manager_ = winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
    xaml_source_ = winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource();
    island_native_ = xaml_source_.as<IDesktopWindowXamlSourceNative2>();
    winrt::check_hresult(island_native_->AttachToWindow(window_));
    winrt::check_hresult(island_native_->get_WindowHandle(&island_window_));
    resize_island();
}

void SettingsMenuWindow::build_content() {
    using namespace winrt::Windows::UI::Xaml;
    using namespace winrt::Windows::UI::Xaml::Controls;
    using namespace winrt::Windows::UI::Xaml::Media;

    const bool dark = system_uses_dark_theme();
    const Color surface = dark ? color(44, 44, 44) : color(249, 249, 249);
    const Color border_color = dark ? color(72, 72, 72) : color(224, 224, 224);
    const Color text_color = dark ? color(255, 255, 255) : color(32, 32, 32);

    Border root;
    root.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
    root.Background(brush(surface));
    root.BorderBrush(brush(border_color));
    root.BorderThickness(Thickness{1, 1, 1, 1});
    root.CornerRadius(CornerRadius{8, 8, 8, 8});
    root.Padding(Thickness{4, 4, 4, 4});

    Button button;
    button.HorizontalAlignment(HorizontalAlignment::Stretch);
    button.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    button.VerticalContentAlignment(VerticalAlignment::Center);
    button.Height(42);
    button.Padding(Thickness{12, 0, 12, 0});
    button.Background(brush(color(0, 0, 0, 0)));
    button.BorderThickness(Thickness{0, 0, 0, 0});
    button.CornerRadius(CornerRadius{4, 4, 4, 4});

    StackPanel row;
    row.Orientation(Orientation::Horizontal);
    row.VerticalAlignment(VerticalAlignment::Center);

    FontIcon icon;
    icon.Glyph(L"\xE713");
    icon.FontFamily(FontFamily(font_available(L"Segoe Fluent Icons")
                                   ? L"Segoe Fluent Icons"
                                   : L"Segoe MDL2 Assets"));
    icon.FontSize(16);
    icon.Width(28);
    icon.HorizontalAlignment(HorizontalAlignment::Left);
    icon.Foreground(brush(text_color));

    TextBlock label;
    label.Text(L"設定");
    label.FontFamily(FontFamily(L"Microsoft JhengHei UI"));
    label.FontSize(14);
    label.VerticalAlignment(VerticalAlignment::Center);
    label.Foreground(brush(text_color));

    row.Children().Append(icon);
    row.Children().Append(label);
    button.Content(row);
    button.Click([this](const auto&, const auto&) {
        hide();
        if (open_settings_) open_settings_();
    });

    root.Child(button);
    settings_button_ = button;
    xaml_source_.Content(root);
}

void SettingsMenuWindow::resize_island() const noexcept {
    if (!window_ || !island_window_) return;

    RECT client{};
    GetClientRect(window_, &client);
    SetWindowPos(island_window_, nullptr, 0, 0, client.right - client.left,
                 client.bottom - client.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

void SettingsMenuWindow::apply_round_region(int width, int height) const noexcept {
    if (!window_ || width <= 0 || height <= 0) return;

    const UINT dpi = GetDpiForWindow(window_);
    const int radius = scale(menu_corner_radius_dip * 2, dpi);
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, radius, radius);
    if (!region) return;
    if (!SetWindowRgn(window_, region, TRUE)) DeleteObject(region);
}

void SettingsMenuWindow::close_xaml() noexcept {
    settings_button_ = nullptr;
    if (xaml_source_) {
        try {
            xaml_source_.Content(nullptr);
            xaml_source_.Close();
        } catch (...) {
        }
    }
    island_native_ = nullptr;
    xaml_source_ = nullptr;
    xaml_manager_ = nullptr;
    island_window_ = nullptr;
}

}  // namespace llavon::settings
