#include "debugger_window.hpp"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <winrt/Windows.UI.Text.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.h>

namespace llavon::debugger {
namespace {

using namespace winrt::Windows::UI::Text;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

constexpr wchar_t window_class_name[] = L"LlavonImeDebuggerWindow";
constexpr UINT connection_message = WM_APP + 1;
constexpr UINT log_message = WM_APP + 2;
constexpr std::size_t maximum_log_characters = 200'000;
constexpr std::size_t latency_window_size = 120;

TextBlock make_text(const wchar_t* text, double size, FontWeight weight = FontWeights::Normal()) {
    TextBlock block;
    block.Text(text);
    block.FontFamily(FontFamily(L"Segoe UI Variable Text, Microsoft JhengHei UI"));
    block.FontSize(size);
    block.FontWeight(weight);
    return block;
}

std::wstring utf8_to_wide(const std::string& text) {
    if (text.empty()) return {};
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) return L"<invalid UTF-8>";
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                        static_cast<int>(text.size()), result.data(), required);
    return result;
}

double percentile(std::vector<double> values, std::size_t percent) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[((values.size() - 1) * percent) / 100];
}

}  // namespace

DebuggerWindow::~DebuggerWindow() {
    destroy();
}

bool DebuggerWindow::create(HINSTANCE instance) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = window_class_name;
    window_class.hIconSm = window_class.hIcon;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;

    window_ = CreateWindowExW(0, window_class_name, L"Llavon IME Debugger",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              980, 700, nullptr, nullptr, instance, this);
    if (!window_) return false;
    try {
        initialize_xaml();
        build_page();
        start_server();
    } catch (...) {
        destroy();
        throw;
    }
    return true;
}

void DebuggerWindow::show() const noexcept {
    if (!window_) return;
    ShowWindow(window_, SW_SHOWNORMAL);
    UpdateWindow(window_);
}

void DebuggerWindow::destroy() noexcept {
    server_.reset();
    discard_pending_messages();
    close_xaml();
    if (window_) {
        const HWND window = window_;
        DestroyWindow(window);
        if (window_ == window) window_ = nullptr;
    }
}

bool DebuggerWindow::pretranslate(MSG& message) const {
    if (!island_native_) return false;
    BOOL handled = FALSE;
    winrt::check_hresult(island_native_->PreTranslateMessage(&message, &handled));
    return handled != FALSE;
}

LRESULT CALLBACK DebuggerWindow::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    DebuggerWindow* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<DebuggerWindow*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<DebuggerWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->handle_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT DebuggerWindow::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == connection_message) {
        update_connection_count(static_cast<int>(lparam));
        return 0;
    }
    if (message == log_message) {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string*>(lparam));
        if (text) append_message(std::move(*text));
        return 0;
    }
    switch (message) {
        case WM_SIZE:
            resize_island();
            return 0;
        case WM_DPICHANGED: {
            const auto suggested = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_DESTROY:
            server_.reset();
            discard_pending_messages();
            close_xaml();
            PostQuitMessage(0);
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

void DebuggerWindow::initialize_xaml() {
    xaml_manager_ = Hosting::WindowsXamlManager::InitializeForCurrentThread();
    xaml_source_ = Hosting::DesktopWindowXamlSource();
    island_native_ = xaml_source_.as<IDesktopWindowXamlSourceNative2>();
    winrt::check_hresult(island_native_->AttachToWindow(window_));
    winrt::check_hresult(island_native_->get_WindowHandle(&island_window_));
    resize_island();
}

void DebuggerWindow::build_page() {
    Grid shell;
    shell.Padding(Thickness{24, 20, 24, 24});
    RowDefinition header_row;
    header_row.Height(GridLength{1, GridUnitType::Auto});
    shell.RowDefinitions().Append(header_row);
    RowDefinition log_row;
    log_row.Height(GridLength{1, GridUnitType::Star});
    shell.RowDefinitions().Append(log_row);

    StackPanel header;
    header.Spacing(6);
    header.Margin(Thickness{0, 0, 0, 16});
    header.Children().Append(make_text(L"End-to-end diagnostics", 22, FontWeights::SemiBold()));
    connection_status_ = make_text(L"Waiting for producers...", 13);
    e2e_latency_status_ = make_text(L"End-to-end: no samples", 16, FontWeights::SemiBold());
    inference_latency_status_ = make_text(L"Inference: no samples", 13);
    header.Children().Append(connection_status_);
    header.Children().Append(e2e_latency_status_);
    header.Children().Append(inference_latency_status_);
    Grid::SetRow(header, 0);
    shell.Children().Append(header);

    log_output_ = TextBox();
    log_output_.IsReadOnly(true);
    log_output_.AcceptsReturn(true);
    log_output_.TextWrapping(TextWrapping::NoWrap);
    ScrollViewer::SetHorizontalScrollBarVisibility(log_output_, ScrollBarVisibility::Auto);
    ScrollViewer::SetVerticalScrollBarVisibility(log_output_, ScrollBarVisibility::Auto);
    log_output_.FontFamily(FontFamily(L"Cascadia Mono, Consolas"));
    log_output_.FontSize(12);
    Grid::SetRow(log_output_, 1);
    shell.Children().Append(log_output_);
    xaml_source_.Content(shell);
}

void DebuggerWindow::start_server() {
    const HWND target = window_;
    server_ = std::make_unique<PipeServer>(
        [target](int delta) { PostMessageW(target, connection_message, 0, delta); },
        [target](std::string message) {
            auto text = std::make_unique<std::string>(std::move(message));
            if (PostMessageW(target, log_message, 0, reinterpret_cast<LPARAM>(text.get()))) text.release();
        });
}

void DebuggerWindow::update_connection_count(int delta) {
    connection_count_ = std::max(0, connection_count_ + delta);
    if (connection_count_ == 0) {
        connection_status_.Text(L"Waiting for producers...");
    } else {
        connection_status_.Text(
            (std::to_wstring(connection_count_) + L" producer process(es) connected").c_str());
    }
}

void DebuggerWindow::append_message(std::string message) {
    update_latency(message);
    log_text_ += utf8_to_wide(message);
    log_text_ += L"\r\n";
    if (log_text_.size() > maximum_log_characters) {
        const auto line = log_text_.find(L'\n', log_text_.size() - maximum_log_characters);
        log_text_.erase(0, line == std::wstring::npos
                               ? log_text_.size() - maximum_log_characters
                               : line + 1);
    }
    log_output_.Text(log_text_);
    log_output_.Select(static_cast<std::int32_t>(log_text_.size()), 0);
}

void DebuggerWindow::update_latency(const std::string& message) {
    const auto update_metric = [&message](std::string_view marker, std::wstring_view label,
                                          std::deque<double>& recent,
                                          const TextBlock& status) {
        const auto marker_position = message.find(marker);
        if (marker_position == std::string::npos) return false;
        const char* begin = message.data() + marker_position + marker.size();
        const char* end = message.data() + message.size();
        double latency_ms = 0.0;
        if (std::from_chars(begin, end, latency_ms).ec != std::errc{}) return false;

        recent.push_back(latency_ms);
        if (recent.size() > latency_window_size) recent.pop_front();
        const std::vector<double> samples(recent.begin(), recent.end());
        std::wostringstream text;
        text << std::fixed << std::setprecision(3) << label << L": Last " << latency_ms
             << L" ms  |  p50 " << percentile(samples, 50) << L" ms  |  p95 "
             << percentile(samples, 95) << L" ms  |  n=" << samples.size();
        status.Text(text.str());
        return true;
    };

    if (update_metric("[TIME] frontend_e2e_ms=", L"End-to-end", recent_e2e_latency_ms_,
                      e2e_latency_status_)) return;
    update_metric("[TIME] predict_ms=", L"Inference", recent_inference_latency_ms_,
                  inference_latency_status_);
}

void DebuggerWindow::resize_island() const noexcept {
    if (!window_ || !island_window_) return;
    RECT client{};
    if (GetClientRect(window_, &client)) {
        SetWindowPos(island_window_, nullptr, 0, 0, client.right - client.left,
                     client.bottom - client.top,
                     SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
    }
}

void DebuggerWindow::close_xaml() noexcept {
    island_native_ = nullptr;
    island_window_ = nullptr;
    try {
        if (xaml_source_) xaml_source_.Close();
        xaml_source_ = nullptr;
        if (xaml_manager_) xaml_manager_.Close();
        xaml_manager_ = nullptr;
    } catch (...) {
    }
}

void DebuggerWindow::discard_pending_messages() noexcept {
    if (!window_) return;
    MSG message{};
    while (PeekMessageW(&message, window_, log_message, log_message, PM_REMOVE)) {
        delete reinterpret_cast<std::string*>(message.lParam);
    }
}

}  // namespace llavon::debugger
