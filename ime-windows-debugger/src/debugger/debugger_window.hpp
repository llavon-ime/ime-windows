#pragma once

#include "pipe_server.hpp"

#include <windows.h>
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/base.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>

namespace llavon::debugger {

class DebuggerWindow final {
public:
    ~DebuggerWindow();

    bool create(HINSTANCE instance);
    void show() const noexcept;
    void destroy() noexcept;
    bool pretranslate(MSG& message) const;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    void initialize_xaml();
    void build_page();
    void start_server();
    void update_connection_count(int delta);
    void append_message(std::string message);
    void update_latency(const std::string& message);
    void resize_island() const noexcept;
    void close_xaml() noexcept;
    void discard_pending_messages() noexcept;

    HWND window_ = nullptr;
    HWND island_window_ = nullptr;
    winrt::Windows::UI::Xaml::Hosting::WindowsXamlManager xaml_manager_{nullptr};
    winrt::Windows::UI::Xaml::Hosting::DesktopWindowXamlSource xaml_source_{nullptr};
    winrt::com_ptr<IDesktopWindowXamlSourceNative2> island_native_;
    winrt::Windows::UI::Xaml::Controls::TextBlock connection_status_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock e2e_latency_status_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock inference_latency_status_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBox log_output_{nullptr};
    std::unique_ptr<PipeServer> server_;
    std::deque<double> recent_e2e_latency_ms_;
    std::deque<double> recent_inference_latency_ms_;
    std::wstring log_text_;
    int connection_count_ = 0;
};

}  // namespace llavon::debugger
