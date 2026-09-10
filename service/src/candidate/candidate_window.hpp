#pragma once

#include <windows.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/Windows.UI.h>
#include <winrt/base.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "candidate_log.hpp"
#include "window.hpp"

namespace llavon::candidate {

class CandidateWindow final : public Window {
public:
    ~CandidateWindow() override {
        uwp_popup_.close();
    }

    void set_uwp_xaml_popup(bool enabled) {
        if (use_uwp_xaml_popup_ == enabled) {
            return;
        }

        hide();
        if (enabled) {
            destroy();
        } else {
            uwp_popup_.close();
        }
        use_uwp_xaml_popup_ = enabled;
    }

    void hide() noexcept {
        if (use_uwp_xaml_popup_) {
            uwp_popup_.hide();
            return;
        }
        Window::hide();
    }

    void set_owner_window(HWND owner_window) noexcept {
        if (owner_window != nullptr && !IsWindow(owner_window)) {
            owner_window = nullptr;
        }
        if (owner_window_ == owner_window) {
            return;
        }

        owner_window_ = owner_window;
        if (!created()) {
            return;
        }

        SetLastError(ERROR_SUCCESS);
        const LONG_PTR previous =
            SetWindowLongPtrW(hwnd(), GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner_window_));
        const DWORD error = GetLastError();
        if (previous == 0 && error != ERROR_SUCCESS) {
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::set_owner_window failed err=" + std::to_wstring(error));
            return;
        }

        DebugSink::instance().send(
            L"UI", L"CandidateWindow::set_owner_window owner=" +
                       std::to_wstring(reinterpret_cast<ULONG_PTR>(owner_window_)));
    }

    void set_layout_columns(std::size_t columns) {
        layout_columns_ = std::clamp<std::size_t>(columns, 1, max_layout_columns);
        if (number_column_ >= layout_columns_) {
            number_column_ = 0;
        }
        DebugSink::instance().send(L"UI", L"CandidateWindow::set_layout_columns " + std::to_wstring(layout_columns_));
    }

    void set_number_column(std::size_t column) {
        number_column_ = std::min(column, layout_columns_ - 1);
        DebugSink::instance().send(L"UI", L"CandidateWindow::set_number_column " + std::to_wstring(number_column_));
    }

    void set_page_navigation(bool can_prev_page, bool can_next_page) {
        can_prev_page_ = can_prev_page;
        can_next_page_ = can_next_page;
        DebugSink::instance().send(
            L"UI", L"CandidateWindow::set_page_navigation prev=" + std::wstring(can_prev_page_ ? L"TRUE" : L"FALSE") +
                       L", next=" + std::wstring(can_next_page_ ? L"TRUE" : L"FALSE"));
        render_surface();
    }

    void update_candidates(const std::vector<std::wstring>& values) {
        DebugSink::instance().send(
            L"UI", L"CandidateWindow::update_candidates input_count=" + std::to_wstring(values.size()));
        candidates_ = values;
        if (candidates_.size() > max_visible_candidates) {
            candidates_.resize(max_visible_candidates);
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::update_candidates truncated_count=" + std::to_wstring(candidates_.size()));
        }

        if (candidates_.empty()) {
            selection_index_ = 0;
            clear_surface();
            DebugSink::instance().send(L"UI", L"CandidateWindow::update_candidates empty -> hide");
            hide();
            return;
        }

        if (selection_index_ >= candidates_.size()) {
            selection_index_ = candidates_.size() - 1;
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::update_candidates clamp selection=" + std::to_wstring(selection_index_));
        }

        if (!ensure_window()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::update_candidates ensure_window failed");
            return;
        }

        sync_window_dpi();
        resize_to_layout();
        render_surface();
        invalidate(FALSE);
    }

    void set_selection(std::size_t index) {
        if (candidates_.empty()) {
            selection_index_ = 0;
            DebugSink::instance().send(L"UI", L"CandidateWindow::set_selection ignored: empty candidates");
            return;
        }
        selection_index_ = std::min(index, candidates_.size() - 1);
        DebugSink::instance().send(L"UI", L"CandidateWindow::set_selection index=" + std::to_wstring(index) +
                                              L", effective=" + std::to_wstring(selection_index_));
        render_surface();
        invalidate(FALSE);
    }

    void show_near_cursor() {
        if (candidates_.empty()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::show_near_cursor empty -> hide");
            hide();
            return;
        }
        if (!ensure_window()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::show_near_cursor ensure_window failed");
            return;
        }

        POINT cursor = {};
        if (GetCursorPos(&cursor) == 0) {
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::show_near_cursor GetCursorPos failed err=" + std::to_wstring(GetLastError()));
            return;
        }

        sync_window_dpi();
        const auto [width, height] = client_size();
        DebugSink::instance().send(
            L"UI", L"CandidateWindow::show_near_cursor cursor=(" + std::to_wstring(cursor.x) + L"," +
                       std::to_wstring(cursor.y) + L"), size=(" + std::to_wstring(width) + L"," +
                       std::to_wstring(height) + L")");
        show_at(cursor.x, cursor.y, cursor.y);
    }

    void show_at(int anchorX, int anchorTop, int anchorBottom) {
        if (candidates_.empty()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::show_at empty -> hide");
            hide();
            return;
        }
        if (!ensure_window()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::show_at ensure_window failed");
            return;
        }

        sync_window_dpi();
        const auto [width, height] = client_size();
        int x = anchorX + scale(popup_offset_x);
        int y = anchorBottom + scale(popup_offset_y);

        const POINT anchor = {anchorX, anchorBottom};
        const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo = {};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (GetMonitorInfoW(monitor, &monitorInfo) != 0) {
            if (x + width > monitorInfo.rcWork.right) {
                x = monitorInfo.rcWork.right - width;
            }
            if (x < monitorInfo.rcWork.left) {
                x = monitorInfo.rcWork.left;
            }
            if (y + height > monitorInfo.rcWork.bottom) {
                // Flip above the complete TSF GetTextExt rectangle, not above rc.bottom. Using the bottom edge here
                // makes the candidate window overlap the composition text whenever it is forced upward at the
                // monitor work-area boundary.
                y = anchorTop - height - scale(4);
            }
            if (y < monitorInfo.rcWork.top) {
                y = monitorInfo.rcWork.top;
            }
        }
        DebugSink::instance().send(
            L"UI", L"CandidateWindow::show_at anchorRect=(" + std::to_wstring(anchorX) + L"," +
                       std::to_wstring(anchorTop) + L".." + std::to_wstring(anchorBottom) +
                       L"), final=(" + std::to_wstring(x) + L"," + std::to_wstring(y) + L"), size=(" +
                       std::to_wstring(width) + L"," + std::to_wstring(height) + L")");
        if (use_uwp_xaml_popup_) {
            show_uwp_popup_at(x, y);
            return;
        }
        set_window_pos(HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Window::show(SW_SHOWNOACTIVATE);
    }

private:
    static constexpr UINT default_dpi = USER_DEFAULT_SCREEN_DPI;
    static constexpr int popup_width = 114;
    static constexpr int expanded_column_width = 84;
    static constexpr int column_gap = 12;
    static constexpr int popup_offset_x = 0;
    static constexpr int popup_offset_y = 4;
    static constexpr int content_padding_x = 6;
    static constexpr int content_padding_y = 6;
    static constexpr int row_height = 29;
    static constexpr int bottom_bar_height = 24;
    static constexpr int corner_radius = 9;
    // CreateWindowInBand and its band identifiers are undocumented Windows implementation details. Band 16 is used
    // here because diagnostics/window-band-probe empirically established on Windows 11 build 26100 that:
    //   * SearchHost's visible search window is in band 13;
    //   * an unsigned, medium-integrity, TokenUIAccess=0 process can create (but cannot move) a window in band 16;
    //   * a band-16 WS_EX_NOACTIVATE window is above SearchHost, retains foreground focus, and can host/render a
    //     DesktopWindowXamlSource.
    // The numeric value is not a public contract. Window::create_in_band_or_fallback dynamically resolves the API
    // and falls back to ordinary CreateWindowExW whenever it is unavailable or rejected on another Windows build.
    static constexpr DWORD empirically_verified_candidate_window_band = 16;
    static constexpr std::size_t max_visible_candidates = 36;
    static constexpr std::size_t max_layout_columns = 4;
    static constexpr int page_size = 9;

protected:
    const wchar_t* class_name() const noexcept override {
        return L"TSF_CandidatePopupWindow";
    }

    DWORD class_style() const noexcept override {
        return CS_HREDRAW | CS_VREDRAW;
    }

    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam) override {
        switch (message) {
            case WM_DPICHANGED:
                DebugSink::instance().send(L"UI", L"CandidateWindow::handle_message WM_DPICHANGED");
                handle_dpi_changed(wParam, lParam);
                return 0;
            case WM_SIZE:
                DebugSink::instance().send(L"UI", L"CandidateWindow::handle_message WM_SIZE");
                apply_round_region();
                resize_xaml_island();
                return 0;
            case WM_ERASEBKGND:
                DebugSink::instance().send(L"UI", L"CandidateWindow::handle_message WM_ERASEBKGND");
                return 1;
            case WM_PAINT:
                DebugSink::instance().send(L"UI", L"CandidateWindow::handle_message WM_PAINT");
                paint();
                return 0;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
            case WM_SYSCOLORCHANGE:
                DebugSink::instance().send(L"UI", L"CandidateWindow::handle_message theme changed");
                render_surface();
                invalidate(FALSE);
                return 0;
            default:
                break;
        }
        return Window::handle_message(message, wParam, lParam);
    }

    void on_final_destroy() noexcept override {
        xaml_island_.close();
    }

private:
    bool ensure_window() {
        if (use_uwp_xaml_popup_) {
            return uwp_popup_.ensure();
        }

        if (created()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::ensure_window already created");
            return true;
        }

        const auto [width, height] = client_size();
        DebugSink::instance().send(L"UI", L"CandidateWindow::ensure_window creating size=(" + std::to_wstring(width) +
                                               L"," + std::to_wstring(height) + L")");
        if (!create_in_band_or_fallback(empirically_verified_candidate_window_band,
                                        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP,
                                        L"拉風輸入法候選字", CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                        owner_window_)) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::ensure_window create failed");
            return false;
        }

        apply_round_region();
        sync_window_dpi();
        ensure_xaml_island();
        render_surface();
        DebugSink::instance().send(L"UI", L"CandidateWindow::ensure_window created");
        return true;
    }

    std::pair<int, int> client_size() const {
        const int rows = std::max(1, std::min(page_size, static_cast<int>(candidates_.size())));
        const std::size_t columns = std::max<std::size_t>(1, std::min(layout_columns_, max_layout_columns));
        const int width =
            (columns == 1)
                ? scale(popup_width)
                : scale(content_padding_x * 2) + static_cast<int>(columns) * scale(expanded_column_width) +
                      static_cast<int>(columns - 1) * scale(column_gap);
        const int height = scale(content_padding_y * 2) + rows * scale(row_height) + scale(bottom_bar_height);
        DebugSink::instance().send(L"UI", L"CandidateWindow::client_size rows=" + std::to_wstring(rows) + L", width=" +
                                              std::to_wstring(width) + L", height=" + std::to_wstring(height));
        return {width, height};
    }

    void resize_to_layout() {
        if (!created()) {
            return;
        }
        const auto [width, height] = client_size();
        set_window_pos(nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    }

    void apply_round_region() {
        if (!created()) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::apply_round_region ignored: not created");
            return;
        }

        RECT rc = {};
        GetClientRect(hwnd(), &rc);
        const int width = static_cast<int>(rc.right - rc.left);
        const int height = static_cast<int>(rc.bottom - rc.top);
        if (width <= 0 || height <= 0) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::apply_round_region invalid size width=" +
                                                  std::to_wstring(width) + L", height=" + std::to_wstring(height));
            return;
        }

        const int ellipse = scale(corner_radius * 2);
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, ellipse, ellipse);
        if (!region) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::apply_round_region CreateRoundRectRgn failed err=" +
                                                  std::to_wstring(GetLastError()));
            return;
        }
        if (SetWindowRgn(hwnd(), region, TRUE) == 0) {
            DeleteObject(region);
            DebugSink::instance().send(
                L"UI",
                L"CandidateWindow::apply_round_region SetWindowRgn failed err=" + std::to_wstring(GetLastError()));
            return;
        }
        DebugSink::instance().send(L"UI", L"CandidateWindow::apply_round_region success width=" +
                                              std::to_wstring(width) + L", height=" + std::to_wstring(height));
    }

    static UINT dpi_from_wparam(WPARAM wParam) noexcept {
        const UINT high = HIWORD(wParam);
        const UINT low = LOWORD(wParam);
        return high != 0 ? high : (low != 0 ? low : default_dpi);
    }

    int scale(int value) const noexcept {
        return MulDiv(value, static_cast<int>(current_dpi_), static_cast<int>(default_dpi));
    }

    void sync_window_dpi() noexcept {
        const HWND dpi_window = created() ? hwnd() : owner_window_;
        if (dpi_window == nullptr) {
            return;
        }

        const UINT dpi = GetDpiForWindow(dpi_window);
        if (dpi != 0 && dpi != current_dpi_) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::sync_window_dpi old=" + std::to_wstring(current_dpi_) +
                                                  L", new=" + std::to_wstring(dpi));
            current_dpi_ = dpi;
        }
    }

    void handle_dpi_changed(WPARAM wParam, LPARAM lParam) {
        current_dpi_ = dpi_from_wparam(wParam);

        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        const auto [width, height] = client_size();
        if (suggested) {
            set_window_pos(nullptr, suggested->left, suggested->top, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
        } else {
            set_window_pos(nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
        }

        render_surface();
        invalidate(FALSE);
    }

    class XamlIslandHost {
    public:
        bool ensure(HWND parent) {
            if (island_hwnd_ != nullptr) {
                return true;
            }

            const HRESULT apartment_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            if (FAILED(apartment_hr) && apartment_hr != RPC_E_CHANGED_MODE) {
                DebugSink::instance().send(
                    L"UI",
                    L"CandidateWindow::XamlIslandHost CoInitializeEx failed hr=" + std::to_wstring(apartment_hr));
                return false;
            }
            if (apartment_hr == RPC_E_CHANGED_MODE) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::XamlIslandHost CoInitializeEx changed mode; skip XAML island");
                return false;
            }

            apartment_initialized_ = SUCCEEDED(apartment_hr);

            try {
                {
                    if (!xaml_manager_) {
                        xaml_manager_ =
                            winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager::InitializeForCurrentThread();
                    }
                    if (!xaml_source_) {
                        xaml_source_ = winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource();
                    }

                    const auto interop = xaml_source_.as<::IDesktopWindowXamlSourceNative>();
                    winrt::check_hresult(interop->AttachToWindow(parent));

                    HWND island_hwnd = nullptr;
                    winrt::check_hresult(interop->get_WindowHandle(&island_hwnd));
                    island_hwnd_ = island_hwnd;
                }

                DebugSink::instance().send(L"UI", L"CandidateWindow::XamlIslandHost attached hwnd=" +
                                                      std::to_wstring(reinterpret_cast<ULONG_PTR>(island_hwnd_)));
                return true;
            } catch (const winrt::hresult_error& e) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::XamlIslandHost ensure failed hr=" + std::to_wstring(e.code().value) +
                               L", message=" + std::wstring(e.message().c_str()));
            } catch (...) {
                DebugSink::instance().send(L"UI", L"CandidateWindow::XamlIslandHost ensure failed with unknown error");
            }

            close();
            return false;
        }

        bool ready() const noexcept {
            return island_hwnd_ != nullptr && xaml_source_ != nullptr;
        }

        void resize(int width, int height) const noexcept {
            if (island_hwnd_ == nullptr) {
                return;
            }
            const BOOL ok = SetWindowPos(
                island_hwnd_, nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::XamlIslandHost resize width=" + std::to_wstring(width) + L", height=" +
                           std::to_wstring(height) + L", ok=" + std::wstring(ok ? L"TRUE" : L"FALSE"));
        }

        void clear() {
            if (!ready()) {
                return;
            }

            try {
                xaml_source_.Content(winrt::Windows::UI::Xaml::UIElement{nullptr});
            } catch (...) {
                DebugSink::instance().send(L"UI", L"CandidateWindow::XamlIslandHost clear ignored exception");
            }
        }

        void render(const std::vector<std::wstring>& candidates, std::size_t selection_index,
                    std::size_t layout_columns, std::size_t number_column, bool can_prev_page, bool can_next_page) {
            if (!ready()) {
                return;
            }

            try {
                xaml_source_.Content(build_root(
                    candidates, selection_index, layout_columns, number_column, can_prev_page, can_next_page));
            } catch (const winrt::hresult_error& e) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::XamlIslandHost render failed hr=" + std::to_wstring(e.code().value) +
                               L", message=" + std::wstring(e.message().c_str()));
            } catch (...) {
                DebugSink::instance().send(L"UI", L"CandidateWindow::XamlIslandHost render failed with unknown error");
            }
        }

        static winrt::Windows::UI::Xaml::UIElement build_content(
            const std::vector<std::wstring>& candidates, std::size_t selection_index,
            std::size_t layout_columns, std::size_t number_column, bool can_prev_page, bool can_next_page) {
            return build_root(
                candidates, selection_index, layout_columns, number_column, can_prev_page, can_next_page);
        }

        void close() noexcept {
            island_hwnd_ = nullptr;
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
                DebugSink::instance().send(L"UI", L"CandidateWindow::XamlIslandHost close ignored exception");
            }

            if (apartment_initialized_) {
                CoUninitialize();
                apartment_initialized_ = false;
            }
        }

    private:
        struct RgbColor {
            std::uint8_t r;
            std::uint8_t g;
            std::uint8_t b;
        };

        struct ThemePalette {
            RgbColor surface;
            RgbColor border;
            RgbColor selection;
            RgbColor accent;
            RgbColor primary_text;
            RgbColor secondary_text;
            RgbColor disabled_text;
            RgbColor divider;
        };

        static bool system_uses_dark_theme() noexcept {
            try {
                const auto background = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
                    winrt::Windows::UI::ViewManagement::UIColorType::Background);
                // The relative luminance coefficients are scaled to integers. A dark system background
                // means Windows is using its dark app theme (or an accessibility theme that also needs
                // the high-contrast dark palette).
                const unsigned int luminance =
                    2126u * background.R + 7152u * background.G + 722u * background.B;
                return luminance < 128u * 10000u;
            } catch (...) {
                return false;
            }
        }

        static ThemePalette theme_palette(bool dark) noexcept {
            if (dark) {
                return {
                    {32, 32, 32},     // surface
                    {62, 62, 62},     // border
                    {59, 59, 59},     // selection
                    {96, 205, 255},   // accent
                    {245, 245, 245},  // primary text
                    {207, 207, 207},  // secondary text
                    {122, 122, 122},  // disabled text
                    {61, 61, 61},     // divider
                };
            }
            return {
                {248, 248, 248},  // surface
                {221, 221, 221},  // border
                {237, 237, 237},  // selection
                {0, 102, 214},    // accent
                {33, 33, 33},     // primary text
                {96, 96, 96},     // secondary text
                {170, 170, 170},  // disabled text
                {225, 225, 225},  // divider
            };
        }

        static winrt::Windows::UI::Color color(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            return winrt::Windows::UI::Color{255, r, g, b};
        }

        static winrt::Windows::UI::Xaml::Media::SolidColorBrush brush(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
            return winrt::Windows::UI::Xaml::Media::SolidColorBrush(color(r, g, b));
        }

        static winrt::Windows::UI::Xaml::Media::SolidColorBrush brush(const RgbColor& value) {
            return brush(value.r, value.g, value.b);
        }

        static double column_width(std::size_t layout_columns) {
            return (layout_columns <= 1)
                       ? static_cast<double>(CandidateWindow::popup_width - CandidateWindow::content_padding_x * 2)
                       : static_cast<double>(CandidateWindow::expanded_column_width);
        }

        static double candidate_text_width(std::size_t layout_columns, bool show_number) {
            (void)show_number;
            if (layout_columns <= 1) {
                return 56.0;
            }
            return 42.0;
        }

        static winrt::Windows::UI::Xaml::Controls::TextBlock build_text(
            const std::wstring& text, double font_size, const RgbColor& foreground) {
            using namespace winrt::Windows::UI::Text;
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;
            using namespace winrt::Windows::UI::Xaml::Media;

            TextBlock block;
            block.Text(text);
            block.FontFamily(FontFamily(L"Microsoft JhengHei UI"));
            block.FontSize(font_size);
            block.FontWeight(FontWeights::Normal());
            block.Foreground(brush(foreground));
            block.TextTrimming(TextTrimming::CharacterEllipsis);
            block.TextWrapping(TextWrapping::NoWrap);
            block.VerticalAlignment(VerticalAlignment::Center);
            return block;
        }

        static winrt::Windows::UI::Xaml::UIElement build_candidate_item(
            std::size_t layout_columns, bool show_number, bool selected, int row_number, const std::wstring& value,
            const ThemePalette& palette) {
            using namespace winrt::Windows::UI::Text;
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;

            Border host;
            host.Width(column_width(layout_columns));
            host.Height(static_cast<double>(CandidateWindow::row_height - 1));
            host.Margin(Thickness{0.0, 0.0, 0.0, 1.0});
            host.CornerRadius(CornerRadius{5.0, 5.0, 5.0, 5.0});
            host.Background(selected ? brush(palette.selection) : brush(0, 0, 0, 0));

            StackPanel row;
            row.Orientation(Orientation::Horizontal);
            row.VerticalAlignment(VerticalAlignment::Center);

            Border accent;
            accent.Width(2.0);
            accent.Height(15.0);
            accent.Margin(Thickness{6.0, 6.0, 6.0, 6.0});
            accent.CornerRadius(CornerRadius{2.0, 2.0, 2.0, 2.0});
            accent.Background((show_number && selected) ? brush(palette.accent) : brush(0, 0, 0, 0));
            row.Children().Append(accent);

            auto number_text =
                build_text(show_number ? std::to_wstring(row_number) : L"", 14.0, palette.secondary_text);
            number_text.Width(14.0);
            number_text.Margin(Thickness{0.0, 0.0, 6.0, 0.0});
            row.Children().Append(number_text);

            auto value_text = build_text(value, (layout_columns <= 1) ? 17.0 : 16.0, palette.primary_text);
            value_text.Width(candidate_text_width(layout_columns, show_number));
            value_text.FontWeight(selected ? FontWeights::SemiBold() : FontWeights::Normal());
            value_text.Margin(Thickness{0.0, 0.0, 6.0, 0.0});
            row.Children().Append(value_text);

            host.Child(row);
            return host;
        }

        static winrt::Windows::UI::Xaml::UIElement build_column(
            const std::vector<std::wstring>& candidates, std::size_t layout_columns, std::size_t number_column,
            std::size_t selection_index, std::size_t column_index, const ThemePalette& palette) {
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;

            StackPanel column;
            column.Width(column_width(layout_columns));
            if (column_index + 1 < layout_columns) {
                column.Margin(Thickness{0.0, 0.0, static_cast<double>(CandidateWindow::column_gap), 0.0});
            }

            const std::size_t begin = column_index * CandidateWindow::page_size;
            const std::size_t end = std::min(begin + CandidateWindow::page_size, candidates.size());
            const bool show_number = (column_index == number_column);
            for (std::size_t i = begin; i < end; ++i) {
                const int row_number = static_cast<int>(i - begin) + 1;
                column.Children().Append(build_candidate_item(
                    layout_columns, show_number, i == selection_index, row_number, candidates[i], palette));
            }

            return column;
        }

        static winrt::Windows::UI::Xaml::UIElement build_footer_icon(
            const wchar_t* glyph, bool enabled, bool outlined, const ThemePalette& palette) {
            using namespace winrt::Windows::UI::Text;
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;

            auto icon_text = build_text(
                glyph, 9.5, enabled ? palette.primary_text : palette.disabled_text);
            icon_text.FontFamily(winrt::Windows::UI::Xaml::Media::FontFamily(L"Segoe UI Symbol"));
            icon_text.HorizontalAlignment(HorizontalAlignment::Center);
            icon_text.VerticalAlignment(VerticalAlignment::Center);

            Border host;
            host.Width(outlined ? 16.0 : 11.0);
            host.Height(16.0);
            host.Margin(Thickness{0.0, 0.0, 6.0, 0.0});
            host.CornerRadius(CornerRadius{3.0, 3.0, 3.0, 3.0});
            host.BorderThickness(outlined ? Thickness{1.0, 1.0, 1.0, 1.0} : Thickness{0.0, 0.0, 0.0, 0.0});
            host.BorderBrush(brush(enabled ? palette.primary_text : palette.disabled_text));
            host.Child(icon_text);
            return host;
        }

        static winrt::Windows::UI::Xaml::UIElement build_footer(
            std::size_t layout_columns, bool can_prev_page, bool can_next_page, const ThemePalette& palette) {
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;

            Border footer;
            footer.BorderThickness(Thickness{0.0, 1.0, 0.0, 0.0});
            footer.BorderBrush(brush(palette.divider));
            footer.Margin(Thickness{0.0, 3.0, 0.0, 0.0});
            footer.Padding(Thickness{6.0, 4.0, 6.0, 4.0});

            Grid footer_grid;

            StackPanel left_group;
            left_group.Orientation(Orientation::Horizontal);
            left_group.HorizontalAlignment(HorizontalAlignment::Left);
            left_group.Children().Append(
                build_footer_icon(layout_columns <= 1 ? L"\u25B2" : L"\u25C0", can_prev_page, false, palette));
            left_group.Children().Append(
                build_footer_icon(layout_columns <= 1 ? L"\u25BC" : L"\u25B6", can_next_page, false, palette));

            StackPanel right_group;
            right_group.Orientation(Orientation::Horizontal);
            right_group.HorizontalAlignment(HorizontalAlignment::Right);
            right_group.Children().Append(build_footer_icon(L"\u21B5", true, true, palette));
            right_group.Children().Append(build_footer_icon(L"\u2665", true, true, palette));

            footer_grid.Children().Append(left_group);
            footer_grid.Children().Append(right_group);
            footer.Child(footer_grid);
            return footer;
        }

        static winrt::Windows::UI::Xaml::UIElement build_root(
            const std::vector<std::wstring>& candidates, std::size_t selection_index, std::size_t layout_columns,
            std::size_t number_column, bool can_prev_page, bool can_next_page) {
            using namespace winrt::Windows::UI::Xaml;
            using namespace winrt::Windows::UI::Xaml::Controls;

            const bool dark = system_uses_dark_theme();
            const ThemePalette palette = theme_palette(dark);

            Border root;
            root.RequestedTheme(dark ? ElementTheme::Dark : ElementTheme::Light);
            root.Background(brush(palette.surface));
            root.BorderBrush(brush(palette.border));
            root.BorderThickness(Thickness{1.0, 1.0, 1.0, 1.0});
            root.CornerRadius(CornerRadius{10.0, 10.0, 10.0, 10.0});

            StackPanel surface;
            surface.Orientation(Orientation::Vertical);

            StackPanel columns;
            columns.Orientation(Orientation::Horizontal);
            columns.Margin(Thickness{static_cast<double>(CandidateWindow::content_padding_x),
                                     static_cast<double>(CandidateWindow::content_padding_y),
                                     static_cast<double>(CandidateWindow::content_padding_x), 0.0});

            const std::size_t visible_columns =
                std::max<std::size_t>(1, std::min(layout_columns, CandidateWindow::max_layout_columns));
            for (std::size_t column_index = 0; column_index < visible_columns; ++column_index) {
                columns.Children().Append(
                    build_column(candidates, visible_columns, number_column, selection_index, column_index, palette));
            }

            surface.Children().Append(columns);
            surface.Children().Append(build_footer(visible_columns, can_prev_page, can_next_page, palette));
            root.Child(surface);
            return root;
        }

    private:
        static winrt::Windows::UI::Xaml::Media::SolidColorBrush brush(
            std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a) {
            return winrt::Windows::UI::Xaml::Media::SolidColorBrush(winrt::Windows::UI::Color{a, r, g, b});
        }

    private:
        winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager xaml_manager_{nullptr};
        winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{nullptr};
        HWND island_hwnd_ = nullptr;
        bool apartment_initialized_ = false;
    };

    class UwpXamlPopupHost {
    public:
        bool ensure() {
            if (popup_) {
                return true;
            }

            try {
                const auto current_window = winrt::Windows::UI::Xaml::Window::Current();
                if (!current_window || !current_window.Content()) {
                    DebugSink::instance().send(
                        L"UI", L"CandidateWindow::UwpXamlPopupHost no Windows.UI.Xaml Window on current thread");
                    return false;
                }

                winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup;
                popup.IsLightDismissEnabled(false);
                popup.ShouldConstrainToRootBounds(false);

                // Keep Popup.Child stable for the lifetime of the popup. Replacing
                // Popup.Child while it is open makes XAML internally close and
                // reopen the popup, which is visible as a transparency flash.
                winrt::Windows::UI::Xaml::Controls::ContentControl content_host;
                popup.Child(content_host);

                content_host_ = content_host;
                popup_ = popup;
                DebugSink::instance().send(L"UI", L"CandidateWindow::UwpXamlPopupHost created");
                return true;
            } catch (const winrt::hresult_error& e) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost ensure failed hr=" +
                               std::to_wstring(e.code().value) + L", message=" + std::wstring(e.message().c_str()));
            } catch (...) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost ensure failed with unknown error");
            }
            content_host_ = nullptr;
            popup_ = nullptr;
            return false;
        }

        bool ready() const noexcept {
            return popup_ != nullptr && content_host_ != nullptr;
        }

        void render(const std::vector<std::wstring>& candidates, std::size_t selection_index,
                    std::size_t layout_columns, std::size_t number_column, bool can_prev_page, bool can_next_page) {
            if (!ready()) {
                return;
            }

            try {
                content_host_.Content(XamlIslandHost::build_content(
                    candidates, selection_index, layout_columns, number_column, can_prev_page, can_next_page));
            } catch (const winrt::hresult_error& e) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost render failed hr=" +
                               std::to_wstring(e.code().value) + L", message=" + std::wstring(e.message().c_str()));
            } catch (...) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost render failed with unknown error");
            }
        }

        void show_at(double horizontal_offset, double vertical_offset) {
            if (!ready()) {
                return;
            }

            try {
                popup_.HorizontalOffset(horizontal_offset);
                popup_.VerticalOffset(vertical_offset);
                popup_.IsOpen(true);
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost shown offset=(" +
                               std::to_wstring(horizontal_offset) + L"," + std::to_wstring(vertical_offset) + L")");
            } catch (const winrt::hresult_error& e) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost show failed hr=" +
                               std::to_wstring(e.code().value) + L", message=" + std::wstring(e.message().c_str()));
            } catch (...) {
                DebugSink::instance().send(
                    L"UI", L"CandidateWindow::UwpXamlPopupHost show failed with unknown error");
            }
        }

        void hide() noexcept {
            try {
                if (popup_) {
                    popup_.IsOpen(false);
                }
            } catch (...) {
                DebugSink::instance().send(L"UI", L"CandidateWindow::UwpXamlPopupHost hide ignored exception");
            }
        }

        void clear() noexcept {
            try {
                if (content_host_) {
                    content_host_.Content(nullptr);
                }
            } catch (...) {
                DebugSink::instance().send(L"UI", L"CandidateWindow::UwpXamlPopupHost clear ignored exception");
            }
        }

        void close() noexcept {
            hide();
            clear();
            if (popup_) {
                try {
                    popup_.Child(winrt::Windows::UI::Xaml::UIElement{nullptr});
                } catch (...) {
                    DebugSink::instance().send(
                        L"UI", L"CandidateWindow::UwpXamlPopupHost close ignored child exception");
                }
            }
            content_host_ = nullptr;
            popup_ = nullptr;
        }

    private:
        winrt::Windows::UI::Xaml::Controls::Primitives::Popup popup_{nullptr};
        winrt::Windows::UI::Xaml::Controls::ContentControl content_host_{nullptr};
    };

    void show_uwp_popup_at(int screen_x, int screen_y) {
        HWND coordinate_window = owner_window_;
        if (coordinate_window != nullptr) {
            const HWND root_window = GetAncestor(coordinate_window, GA_ROOT);
            if (root_window != nullptr) {
                coordinate_window = root_window;
            }
        }
        if (coordinate_window == nullptr) {
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::show_uwp_popup_at missing owner window");
            return;
        }

        POINT client_point = {screen_x, screen_y};
        if (!ScreenToClient(coordinate_window, &client_point)) {
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::show_uwp_popup_at ScreenToClient failed err=" +
                           std::to_wstring(GetLastError()));
            return;
        }

        const double dip_scale = static_cast<double>(default_dpi) / static_cast<double>(current_dpi_);
        uwp_popup_.show_at(
            static_cast<double>(client_point.x) * dip_scale, static_cast<double>(client_point.y) * dip_scale);
    }

    void ensure_xaml_island() {
        if (!created()) {
            return;
        }
        if (!xaml_island_.ensure(hwnd())) {
            DebugSink::instance().send(L"UI", L"CandidateWindow::ensure_xaml_island skipped");
            return;
        }
        resize_xaml_island();
    }

    void resize_xaml_island() {
        if (!created()) {
            return;
        }

        RECT rc = {};
        GetClientRect(hwnd(), &rc);
        const LONG raw_width = rc.right - rc.left;
        const LONG raw_height = rc.bottom - rc.top;
        const int width = (raw_width > 0) ? static_cast<int>(raw_width) : 0;
        const int height = (raw_height > 0) ? static_cast<int>(raw_height) : 0;
        if (width == 0 || height == 0) {
            return;
        }

        xaml_island_.resize(width, height);
    }

    void render_surface() {
        if (candidates_.empty()) {
            return;
        }

        if (use_uwp_xaml_popup_) {
            if (uwp_popup_.ready()) {
                uwp_popup_.render(
                    candidates_, selection_index_, layout_columns_, number_column_, can_prev_page_, can_next_page_);
            }
            return;
        }

        if (!xaml_island_.ready()) {
            return;
        }
        xaml_island_.render(
            candidates_, selection_index_, layout_columns_, number_column_, can_prev_page_, can_next_page_);
    }

    void clear_surface() {
        if (use_uwp_xaml_popup_) {
            uwp_popup_.clear();
        } else {
            xaml_island_.clear();
        }
    }

    void paint() {
        PAINTSTRUCT ps = {};
        HDC hdc = BeginPaint(hwnd(), &ps);
        if (!hdc) {
            DebugSink::instance().send(
                L"UI", L"CandidateWindow::paint BeginPaint failed err=" + std::to_wstring(GetLastError()));
            return;
        }
        EndPaint(hwnd(), &ps);
        DebugSink::instance().send(L"UI", L"CandidateWindow::paint end");
    }

private:
    std::vector<std::wstring> candidates_;
    std::size_t selection_index_ = 0;
    std::size_t layout_columns_ = 1;
    std::size_t number_column_ = 0;
    bool can_prev_page_ = false;
    bool can_next_page_ = false;
    bool use_uwp_xaml_popup_ = false;
    UINT current_dpi_ = default_dpi;
    HWND owner_window_ = nullptr;
    XamlIslandHost xaml_island_;
    UwpXamlPopupHost uwp_popup_;
};

}  // namespace llavon::candidate
