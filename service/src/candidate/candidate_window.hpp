#pragma once

#include <dwmapi.h>
#include <windows.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Interop.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>
#include <winrt/base.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "../ui/xaml_resource.hpp"
#include "candidate_resources.h"
#include "window.hpp"

namespace llavon::candidate {

class CandidateWindow final : public Window {
public:
    explicit CandidateWindow(llavon::ime::core::Logger& logger) noexcept : Window(logger) {}

    ~CandidateWindow() override {
        destroy();
        close_xaml();
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
            logger_->log(LogInformation::debug, [error = error] {
                return std::format("[UI] CandidateWindow::set_owner_window failed err={}", error);
            });
            return;
        }

        logger_->log(LogInformation::debug, [owner = reinterpret_cast<ULONG_PTR>(owner_window_)] {
            return std::format("[UI] CandidateWindow::set_owner_window owner={}", owner);
        });
    }

    void set_layout_columns(std::size_t columns) {
        layout_columns_ = std::clamp<std::size_t>(columns, 1, max_layout_columns);
        if (number_column_ >= layout_columns_) {
            number_column_ = 0;
        }
        logger_->log(LogInformation::debug, [columns = layout_columns_] {
            return std::format("[UI] CandidateWindow::set_layout_columns {}", columns);
        });
    }

    void set_number_column(std::size_t column) {
        number_column_ = std::min(column, layout_columns_ - 1);
        logger_->log(LogInformation::debug, [column = number_column_] {
            return std::format("[UI] CandidateWindow::set_number_column {}", column);
        });
    }

    void set_page_navigation(bool can_prev_page, bool can_next_page) {
        can_prev_page_ = can_prev_page;
        can_next_page_ = can_next_page;
        logger_->log(LogInformation::debug, [previous = (can_prev_page_ != 0), next = (can_next_page_ != 0)] {
            return std::format("[UI] CandidateWindow::set_page_navigation prev={}, next={}", previous, next);
        });
    }

    void update_candidates(const std::vector<std::wstring>& values) {
        logger_->log(LogInformation::debug, [count = values.size()] {
            return std::format("[UI] CandidateWindow::update_candidates input_count={}", count);
        });
        candidates_ = values;
        if (candidates_.size() > max_visible_candidates) {
            candidates_.resize(max_visible_candidates);
            logger_->log(LogInformation::debug, [count = candidates_.size()] {
                return std::format("[UI] CandidateWindow::update_candidates truncated_count={}", count);
            });
        }

        if (candidates_.empty()) {
            selection_index_ = 0;
            clear_surface();
            logger_->log(LogInformation::debug, "[UI] CandidateWindow::update_candidates empty -> hide");
            hide();
            return;
        }

        if (selection_index_ >= candidates_.size()) {
            selection_index_ = candidates_.size() - 1;
            logger_->log(LogInformation::debug, [selection = selection_index_] {
                return std::format("[UI] CandidateWindow::update_candidates clamp selection={}", selection);
            });
        }

        if (!ensure_window()) {
            logger_->log(LogInformation::debug,
                         "[UI] CandidateWindow::update_candidates ensure_window failed");
            return;
        }

        sync_window_dpi();
        render_surface();
        resize_to_layout();
        invalidate(FALSE);
    }

    void set_selection(std::size_t index) {
        if (candidates_.empty()) {
            selection_index_ = 0;
            logger_->log(LogInformation::debug,
                         "[UI] CandidateWindow::set_selection ignored: empty candidates");
            return;
        }
        selection_index_ = std::min(index, candidates_.size() - 1);
        logger_->log(LogInformation::debug, [index = index, selection = selection_index_] {
            return std::format("[UI] CandidateWindow::set_selection index={}, effective={}", index,
                               selection);
        });
        render_surface();
        invalidate(FALSE);
    }

    void show_near_cursor() {
        if (candidates_.empty()) {
            logger_->log(LogInformation::debug, "[UI] CandidateWindow::show_near_cursor empty -> hide");
            hide();
            return;
        }
        if (!ensure_window()) {
            logger_->log(LogInformation::debug,
                         "[UI] CandidateWindow::show_near_cursor ensure_window failed");
            return;
        }

        POINT cursor = {};
        if (GetCursorPos(&cursor) == 0) {
            logger_->log(LogInformation::debug, [error = GetLastError()] {
                return std::format("[UI] CandidateWindow::show_near_cursor GetCursorPos failed err={}",
                                   error);
            });
            return;
        }

        sync_window_dpi();
        const auto [width, height] = client_size();
        logger_->log(LogInformation::debug, [cursor_x = cursor.x, cursor_y = cursor.y, width = width,
                                             height = height] {
            return std::format("[UI] CandidateWindow::show_near_cursor cursor=({},{}), size=({},{})",
                               cursor_x, cursor_y, width, height);
        });
        show_at(cursor.x, cursor.y, cursor.y);
    }

    void show_at(int anchorX, int anchorTop, int anchorBottom) {
        if (candidates_.empty()) {
            logger_->log(LogInformation::debug, "[UI] CandidateWindow::show_at empty -> hide");
            hide();
            return;
        }
        if (!ensure_window()) {
            logger_->log(LogInformation::debug, "[UI] CandidateWindow::show_at ensure_window failed");
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
                // Flip above the complete TSF GetTextExt rectangle, not above
                // rc.bottom. Using the bottom edge here makes the candidate window
                // overlap the composition text whenever it is forced upward at the
                // monitor work-area boundary.
                y = anchorTop - height - scale(4);
            }
            if (y < monitorInfo.rcWork.top) {
                y = monitorInfo.rcWork.top;
            }
        }
        logger_->log(LogInformation::debug, [anchor_x = anchorX, anchor_top = anchorTop,
                                             anchor_bottom = anchorBottom, x = x, y = y, width = width,
                                             height = height] {
            return std::format(
                "[UI] CandidateWindow::show_at anchorRect=({},{}..{}), final=({},{}), size=({},{})", anchor_x,
                anchor_top, anchor_bottom, x, y, width, height);
        });
        set_window_pos(HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        Window::show(SW_SHOWNOACTIVATE);
    }

private:
    static constexpr UINT default_dpi = USER_DEFAULT_SCREEN_DPI;
    static constexpr int popup_offset_x = 0;
    static constexpr int popup_offset_y = 4;
    // CreateWindowInBand and its band identifiers are undocumented Windows
    // implementation details. Band 16 is used here because
    // diagnostics/window-band-probe empirically established on Windows 11 build
    // 26100 that:
    //   * SearchHost's visible search window is in band 13;
    //   * an unsigned, medium-integrity, TokenUIAccess=0 process can create (but
    //   cannot move) a window in band 16;
    //   * a band-16 WS_EX_NOACTIVATE window is above SearchHost, retains
    //   foreground focus, and can host/render a
    //     DesktopWindowXamlSource.
    // The numeric value is not a public contract.
    // Window::create_in_band_or_fallback dynamically resolves the API and falls
    // back to ordinary CreateWindowExW whenever it is unavailable or rejected on
    // another Windows build.
    static constexpr DWORD empirically_verified_candidate_window_band = 16;
    static constexpr std::size_t max_visible_candidates = 36;
    static constexpr std::size_t max_layout_columns = 4;
    static constexpr int page_size = 9;

protected:
    const wchar_t* class_name() const noexcept override { return L"TSF_CandidatePopupWindow"; }

    DWORD class_style() const noexcept override { return CS_HREDRAW | CS_VREDRAW; }

    LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam) override {
        try {
            switch (message) {
            case WM_MOUSEACTIVATE:
                return MA_NOACTIVATE;
            case WM_DESTROY:
                close_xaml();
                return 0;
            case WM_DPICHANGED:
                logger_->log(LogInformation::debug, "[UI] CandidateWindow::handle_message WM_DPICHANGED");
                handle_dpi_changed(wParam, lParam);
                return 0;
            case WM_SIZE:
                logger_->log(LogInformation::debug, "[UI] CandidateWindow::handle_message WM_SIZE");
                if (!uses_system_rounded_corners_) {
                    apply_round_region();
                }
                resize_xaml_island();
                return 0;
            case WM_ERASEBKGND:
                logger_->log(LogInformation::debug, "[UI] CandidateWindow::handle_message WM_ERASEBKGND");
                return 1;
            case WM_PAINT:
                logger_->log(LogInformation::debug, "[UI] CandidateWindow::handle_message WM_PAINT");
                paint();
                return 0;
            case WM_SETTINGCHANGE:
            case WM_THEMECHANGED:
            case WM_SYSCOLORCHANGE:
                logger_->log(LogInformation::debug, "[UI] CandidateWindow::handle_message theme changed");
                render_surface();
                invalidate(FALSE);
                return 0;
            default:
                break;
            }
        } catch (const winrt::hresult_error& error) {
            logger_->log(LogInformation::debug, [message = error.message()] {
                return std::format("[ERROR] Candidate window update failed: {}", winrt::to_string(message));
            });
            hide();
        } catch (...) {
            logger_->log(LogInformation::debug,
                         "[ERROR] Candidate window update failed with an unknown error");
            hide();
        }
        return Window::handle_message(message, wParam, lParam);
    }

    void on_final_destroy() noexcept override {
        uses_system_rounded_corners_ = false;
        close_xaml();
    }

private:
    bool ensure_window() {
        if (created()) {
            logger_->log(LogInformation::debug, "[UI] CandidateWindow::ensure_window already created");
            return true;
        }

        const auto [width, height] = client_size();
        logger_->log(LogInformation::debug, [width = width, height = height] {
            return std::format("[UI] CandidateWindow::ensure_window creating size=({},{})", width, height);
        });
        if (!create_in_band_or_fallback(empirically_verified_candidate_window_band,
                                        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WS_POPUP,
                                        L"拉風輸入法候選字", CW_USEDEFAULT, CW_USEDEFAULT, width, height,
                                        owner_window_)) {
            logger_->log(LogInformation::debug, "[UI] CandidateWindow::ensure_window create failed");
            return false;
        }

        uses_system_rounded_corners_ = enable_system_rounded_corners();
        if (!uses_system_rounded_corners_) {
            apply_round_region();
        }
        sync_window_dpi();
        try {
            initialize_xaml_island();
        } catch (const winrt::hresult_error& error) {
            logger_->log(LogInformation::debug, [message = error.message()] {
                return std::format("[ERROR] WinUI island initialization failed: {}",
                                   winrt::to_string(message));
            });
            destroy();
            return false;
        }
        render_surface();
        logger_->log(LogInformation::debug, "[UI] CandidateWindow::ensure_window created");
        return true;
    }

    std::pair<int, int> client_size() const {
        if (!root_)
            return {scale(114), scale(70)};
        constexpr float unlimited = std::numeric_limits<float>::infinity();
        root_.Measure({unlimited, unlimited});
        const auto desired = root_.DesiredSize();
        const double dpi_scale = static_cast<double>(current_dpi_) / default_dpi;
        return {std::max(1, static_cast<int>(std::ceil(desired.Width * dpi_scale))),
                std::max(1, static_cast<int>(std::ceil(desired.Height * dpi_scale)))};
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
            logger_->log(LogInformation::debug,
                         "[UI] CandidateWindow::apply_round_region ignored: not created");
            return;
        }

        RECT rc = {};
        GetClientRect(hwnd(), &rc);
        const int width = static_cast<int>(rc.right - rc.left);
        const int height = static_cast<int>(rc.bottom - rc.top);
        if (width <= 0 || height <= 0) {
            logger_->log(LogInformation::debug, [width = width, height = height] {
                return std::format(
                    "[UI] CandidateWindow::apply_round_region invalid size width={}, height={}", width,
                    height);
            });
            return;
        }

        const int ellipse = scale(static_cast<int>(root_ ? root_.CornerRadius().TopLeft * 2 : 20));
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, ellipse, ellipse);
        if (!region) {
            logger_->log(LogInformation::debug, [error = GetLastError()] {
                return std::format(
                    "[UI] CandidateWindow::apply_round_region CreateRoundRectRgn failed err={}", error);
            });
            return;
        }
        if (SetWindowRgn(hwnd(), region, TRUE) == 0) {
            DeleteObject(region);
            logger_->log(LogInformation::debug, [error = GetLastError()] {
                return std::format("[UI] CandidateWindow::apply_round_region SetWindowRgn failed err={}",
                                   error);
            });
            return;
        }
        logger_->log(LogInformation::debug, [width = width, height = height] {
            return std::format("[UI] CandidateWindow::apply_round_region success width={}, height={}", width,
                               height);
        });
    }

    bool enable_system_rounded_corners() {
        if (!created()) {
            return false;
        }

        // DWMWA_WINDOW_CORNER_PREFERENCE (33) is available on Windows 11. Resolve
        // the attribute by value so the Windows 10 target can still fall back to
        // a window region when the compositor does not support native rounding.
        constexpr DWORD window_corner_preference_attribute = 33;
        constexpr DWORD round_corner_preference = 2;
        const HRESULT result =
            DwmSetWindowAttribute(hwnd(), static_cast<DWMWINDOWATTRIBUTE>(window_corner_preference_attribute),
                                  &round_corner_preference, sizeof(round_corner_preference));
        if (FAILED(result)) {
            logger_->log(LogInformation::debug, [result = result] {
                return std::format("[UI] CandidateWindow::enable_system_rounded_corners unavailable hr={}",
                                   result);
            });
            return false;
        }

        // WM_SIZE can install the pixel-snapped fallback region while the HWND is
        // being created. Remove it so DWM supplies an anti-aliased clip and shadow.
        SetWindowRgn(hwnd(), nullptr, TRUE);
        logger_->log(LogInformation::debug, "[UI] CandidateWindow::enable_system_rounded_corners success");
        return true;
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
            logger_->log(LogInformation::debug, [previous_dpi = current_dpi_, dpi = dpi] {
                return std::format("[UI] CandidateWindow::sync_window_dpi old={}, new={}", previous_dpi, dpi);
            });
            current_dpi_ = dpi;
        }
    }

    void handle_dpi_changed(WPARAM wParam, LPARAM lParam) {
        current_dpi_ = dpi_from_wparam(wParam);

        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        const auto [width, height] = client_size();
        if (suggested) {
            set_window_pos(nullptr, suggested->left, suggested->top, width, height,
                           SWP_NOACTIVATE | SWP_NOZORDER);
        } else {
            set_window_pos(nullptr, 0, 0, width, height, SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
        }

        render_surface();
        invalidate(FALSE);
    }

    void initialize_xaml_island() {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        xaml_source_ = Hosting::DesktopWindowXamlSource();
        xaml_source_.Initialize(winrt::Microsoft::UI::GetWindowIdFromWindow(hwnd()));
        root_ = ui::load_xaml_resource(IDR_CANDIDATE_PAGE_XAML).as<Border>();
        columns_ = root_.FindName(L"CandidateColumns").as<StackPanel>();
        previous_page_icon_ = root_.FindName(L"PreviousPageIcon").as<TextBlock>();
        next_page_icon_ = root_.FindName(L"NextPageIcon").as<TextBlock>();
        xaml_source_.Content(root_);
        resize_xaml_island();
    }

    void resize_xaml_island() const noexcept {
        if (!created() || !xaml_source_)
            return;
        RECT client{};
        if (!GetClientRect(hwnd(), &client))
            return;
        try {
            xaml_source_.SiteBridge().MoveAndResize(
                {0, 0, client.right - client.left, client.bottom - client.top});
            xaml_source_.SiteBridge().Show();
        } catch (...) {
            logger_->log(LogInformation::debug, "[ERROR] Unable to resize WinUI candidate island");
        }
    }

    void render_surface() {
        using namespace winrt::Microsoft::UI::Xaml;
        using namespace winrt::Microsoft::UI::Xaml::Controls;
        if (!root_ || candidates_.empty())
            return;

        try {
            const auto background = winrt::Windows::UI::ViewManagement::UISettings().GetColorValue(
                winrt::Windows::UI::ViewManagement::UIColorType::Background);
            const unsigned int luminance = 2126u * background.R + 7152u * background.G + 722u * background.B;
            root_.RequestedTheme(luminance < 128u * 10000u ? ElementTheme::Dark : ElementTheme::Light);
        } catch (...) {
            // Retain the last theme if Windows cannot supply its current colors.
        }

        // Keep the root and existing rows alive while keyboard selection changes.
        if (rendered_candidates_ != candidates_ || rendered_columns_ != layout_columns_) {
            clear_surface();
            const auto column_template =
                root_.Resources().Lookup(winrt::box_value(L"CandidateColumnTemplate")).as<DataTemplate>();
            const auto resources = Application::Current().Resources();
            const auto column_style =
                resources
                    .Lookup(winrt::box_value(layout_columns_ == 1 ? L"CandidateColumnStyle"
                                                                  : L"CandidateExpandedColumnStyle"))
                    .as<Style>();
            for (std::size_t column_index = 0; column_index < layout_columns_; ++column_index) {
                const auto column = column_template.LoadContent().as<StackPanel>();
                column.Style(column_style);
                const std::size_t begin = column_index * page_size;
                const std::size_t end = std::min(begin + page_size, candidates_.size());
                for (std::size_t index = begin; index < end; ++index) {
                    const auto row = ui::load_xaml_resource(IDR_CANDIDATE_ITEM_XAML).as<Grid>();
                    const auto text = row.FindName(L"CandidateText").as<TextBlock>();
                    text.Text(candidates_[index]);
                    rows_.push_back({row.FindName(L"CandidateNumber").as<TextBlock>(),
                                     row.FindName(L"SelectionHighlight").as<Border>(),
                                     row.FindName(L"SelectionAccent").as<Border>(), text});
                    column.Children().Append(row);
                }
                columns_.Children().Append(column);
            }
            rendered_candidates_ = candidates_;
            rendered_columns_ = layout_columns_;
        }

        const auto resources = Application::Current().Resources();
        const auto normal_style = resources.Lookup(winrt::box_value(L"CandidateTextStyle")).as<Style>();
        const auto selected_style =
            resources.Lookup(winrt::box_value(L"CandidateSelectedTextStyle")).as<Style>();
        const auto number_style = resources.Lookup(winrt::box_value(L"CandidateNumberStyle")).as<Style>();
        const auto selected_number_style =
            resources.Lookup(winrt::box_value(L"CandidateSelectedNumberStyle")).as<Style>();
        for (std::size_t index = 0; index < rows_.size(); ++index) {
            const bool numbered = index / page_size == number_column_;
            const bool selected = index == selection_index_;
            rows_[index].number.Text(numbered ? std::to_wstring(index % page_size + 1) : L"");
            rows_[index].highlight.Visibility(selected ? Visibility::Visible : Visibility::Collapsed);
            rows_[index].accent.Visibility(selected && numbered ? Visibility::Visible
                                                                : Visibility::Collapsed);
            rows_[index].text.Style(selected ? selected_style : normal_style);
            rows_[index].number.Style(selected ? selected_number_style : number_style);
        }
        previous_page_icon_.Text(layout_columns_ == 1 ? L"\u25B2" : L"\u25C0");
        next_page_icon_.Text(layout_columns_ == 1 ? L"\u25BC" : L"\u25B6");
        // Disabled page indicators retain a XAML theme-resource brush.
        previous_page_icon_.Opacity(can_prev_page_ ? 1.0 : 0.35);
        next_page_icon_.Opacity(can_next_page_ ? 1.0 : 0.35);
    }

    void clear_surface() {
        if (columns_)
            columns_.Children().Clear();
        rows_.clear();
        rendered_candidates_.clear();
        rendered_columns_ = 0;
    }

    void close_xaml() noexcept {
        if (xaml_source_) {
            try {
                xaml_source_.Content(nullptr);
                xaml_source_.Close();
            } catch (...) {
                logger_->log(LogInformation::debug, "[ERROR] Unable to close WinUI candidate island");
            }
        }
        rows_.clear();
        rendered_candidates_.clear();
        rendered_columns_ = 0;
        previous_page_icon_ = nullptr;
        next_page_icon_ = nullptr;
        columns_ = nullptr;
        root_ = nullptr;
        xaml_source_ = nullptr;
    }

    void paint() const noexcept {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd(), &paint);
        EndPaint(hwnd(), &paint);
    }

    struct CandidateRow {
        winrt::Microsoft::UI::Xaml::Controls::TextBlock number{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Border highlight{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::Border accent{nullptr};
        winrt::Microsoft::UI::Xaml::Controls::TextBlock text{nullptr};
    };

    std::vector<std::wstring> candidates_;
    std::vector<std::wstring> rendered_candidates_;
    std::vector<CandidateRow> rows_;
    std::size_t rendered_columns_ = 0;
    std::size_t selection_index_ = 0;
    std::size_t layout_columns_ = 1;
    std::size_t number_column_ = 0;
    bool can_prev_page_ = false;
    bool can_next_page_ = false;
    bool uses_system_rounded_corners_ = false;
    UINT current_dpi_ = default_dpi;
    HWND owner_window_ = nullptr;
    winrt::Microsoft::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Border root_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::StackPanel columns_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock previous_page_icon_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock next_page_icon_{nullptr};
};

} // namespace llavon::candidate
