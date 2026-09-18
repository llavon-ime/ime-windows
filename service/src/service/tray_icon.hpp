#pragma once

#include <windows.h>
#include <shellapi.h>

#include <atomic>
#include <functional>

namespace llavon::service {

class TrayIcon final {
public:
    using OpenSettingsCallback = std::function<void()>;
    using OpenDebuggerCallback = std::function<void()>;
    using ShowInputModeMenuCallback = std::function<void(POINT)>;

    TrayIcon() = default;
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;
    ~TrayIcon();

    bool create(HINSTANCE instance, OpenSettingsCallback open_settings,
                OpenDebuggerCallback open_debugger,
                ShowInputModeMenuCallback show_input_mode_menu);
    int run_message_loop();
    void notify_server_stopped(int exit_code) const noexcept;

private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);

    bool add_icon();
    void remove_icon() noexcept;
    void open_settings() const;
    void open_debugger() const;
    void show_context_menu(POINT location);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    std::atomic<HWND> notification_window_{nullptr};
    UINT taskbar_created_message_ = 0;
    UINT open_settings_message_ = 0;
    UINT show_input_mode_menu_message_ = 0;
    NOTIFYICONDATAW icon_data_{};
    OpenSettingsCallback open_settings_;
    OpenDebuggerCallback open_debugger_;
    ShowInputModeMenuCallback show_input_mode_menu_;
    bool icon_added_ = false;
    int exit_code_ = 0;
};

}  // namespace llavon::service
