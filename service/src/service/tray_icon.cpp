#include "tray_icon.hpp"

#include "resource.h"

#include <windowsx.h>

#include <utility>

namespace llavon::service {
namespace {

constexpr wchar_t tray_window_class[] = L"LlavonImeServiceTrayWindow";
constexpr wchar_t tray_tooltip[] = L"Llavon 輸入法";
constexpr UINT tray_callback_message = WM_APP + 20;
constexpr UINT server_stopped_message = WM_APP + 21;
constexpr LRESULT shutdown_acknowledged = 0x4c4c4156;
constexpr UINT open_settings_command = 1;
constexpr UINT open_debugger_command = 2;
constexpr UINT_PTR tray_retry_timer = 1;
constexpr UINT tray_retry_interval_ms = 2000;

// Stable identity for the notification-area icon across Explorer restarts.
constexpr GUID tray_icon_guid = {
    0x6c5d7689, 0xf39e, 0x4a8c, {0x91, 0xd7, 0x0f, 0x48, 0x31, 0x3d, 0x29, 0x6a}};

}  // namespace

TrayIcon::~TrayIcon() {
    remove_icon();
    if (window_) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

bool TrayIcon::create(HINSTANCE instance, OpenSettingsCallback open_settings,
                      OpenDebuggerCallback open_debugger,
                      ShowInputModeMenuCallback show_input_mode_menu,
                      ShutdownCallback shutdown) {
    instance_ = instance;
    open_settings_ = std::move(open_settings);
    open_debugger_ = std::move(open_debugger);
    show_input_mode_menu_ = std::move(show_input_mode_menu);
    shutdown_ = std::move(shutdown);

    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance_;
    window_class.lpszClassName = tray_window_class;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    taskbar_created_message_ = RegisterWindowMessageW(L"TaskbarCreated");
    open_settings_message_ = RegisterWindowMessageW(L"LlavonIme.OpenSettings");
    show_input_mode_menu_message_ = RegisterWindowMessageW(L"LlavonIme.ShowInputModeMenu");
    shutdown_message_ = RegisterWindowMessageW(L"LlavonIme.Shutdown");
    safe_shutdown_message_ = RegisterWindowMessageW(L"LlavonIme.SafeShutdownV2");
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW, tray_window_class, L"Llavon IME Service",
                              WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance_, this);
    notification_window_.store(window_, std::memory_order_release);
    if (!window_) {
        return false;
    }

    // Shell_NotifyIcon must run after CreateWindowEx has finished. Calling it
    // from WM_CREATE can fail because Explorer cannot address the tray host
    // window until window creation is complete.
    if (!add_icon()) {
        SetTimer(window_, tray_retry_timer, tray_retry_interval_ms, nullptr);
    }
    return true;
}

int TrayIcon::run_message_loop() {
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return exit_code_;
}

void TrayIcon::notify_server_stopped(int exit_code) const noexcept {
    const HWND notification_window = notification_window_.load(std::memory_order_acquire);
    if (notification_window) {
        PostMessageW(notification_window, server_stopped_message, static_cast<WPARAM>(exit_code), 0);
    }
}

LRESULT CALLBACK TrayIcon::window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    TrayIcon* self = nullptr;
    if (message == WM_NCCREATE) {
        const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<TrayIcon*>(create->lpCreateParams);
        self->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<TrayIcon*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    }
    return self ? self->handle_message(message, wparam, lparam)
                : DefWindowProcW(window, message, wparam, lparam);
}

LRESULT TrayIcon::handle_message(UINT message, WPARAM wparam, LPARAM lparam) {
    if ((shutdown_message_ != 0 && message == shutdown_message_) ||
        (safe_shutdown_message_ != 0 && message == safe_shutdown_message_)) {
        if (shutdown_) {
            auto shutdown = std::move(shutdown_);
            shutdown();
        }
        exit_code_ = 0;
        DestroyWindow(window_);
        return shutdown_acknowledged;
    }
    if (open_settings_message_ != 0 && message == open_settings_message_) {
        open_settings();
        return 0;
    }
    if (show_input_mode_menu_message_ != 0 && message == show_input_mode_menu_message_) {
        if (show_input_mode_menu_) {
            const POINT location{
                static_cast<LONG>(static_cast<DWORD>(wparam)),
                static_cast<LONG>(static_cast<DWORD>(lparam)),
            };
            show_input_mode_menu_(location);
        }
        return 0;
    }

    if (message == taskbar_created_message_) {
        icon_added_ = false;
        if (!add_icon()) {
            SetTimer(window_, tray_retry_timer, tray_retry_interval_ms, nullptr);
        }
        return 0;
    }

    switch (message) {
        case WM_CREATE:
            return 0;
        case WM_TIMER:
            if (wparam == tray_retry_timer && add_icon()) {
                KillTimer(window_, tray_retry_timer);
                return 0;
            }
            break;
        case tray_callback_message: {
            const UINT notification = LOWORD(lparam);
            if (notification == NIN_SELECT || notification == NIN_KEYSELECT ||
                notification == WM_LBUTTONDBLCLK) {
                open_settings();
                return 0;
            }
            if (notification == WM_CONTEXTMENU || notification == WM_RBUTTONUP) {
                POINT location{GET_X_LPARAM(wparam), GET_Y_LPARAM(wparam)};
                if (location.x == -1 && location.y == -1) {
                    GetCursorPos(&location);
                }
                show_context_menu(location);
                return 0;
            }
            break;
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == open_settings_command) {
                open_settings();
                return 0;
            }
            if (LOWORD(wparam) == open_debugger_command) {
                open_debugger();
                return 0;
            }
            break;
        case server_stopped_message:
            exit_code_ = static_cast<int>(wparam);
            DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            KillTimer(window_, tray_retry_timer);
            remove_icon();
            notification_window_.store(nullptr, std::memory_order_release);
            window_ = nullptr;
            PostQuitMessage(exit_code_);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

bool TrayIcon::add_icon() {
    if (icon_added_) {
        return true;
    }

    icon_data_ = {};
    icon_data_.cbSize = sizeof(icon_data_);
    icon_data_.hWnd = window_;
    icon_data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_GUID;
    icon_data_.uCallbackMessage = tray_callback_message;
    icon_data_.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_LLAVON_TRAY));
    if (!icon_data_.hIcon) {
        icon_data_.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    icon_data_.guidItem = tray_icon_guid;
    wcscpy_s(icon_data_.szTip, tray_tooltip);

    if (!Shell_NotifyIconW(NIM_ADD, &icon_data_)) {
        return false;
    }
    icon_data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &icon_data_);
    icon_added_ = true;
    return true;
}

void TrayIcon::remove_icon() noexcept {
    if (icon_added_) {
        Shell_NotifyIconW(NIM_DELETE, &icon_data_);
    }
    icon_added_ = false;
    icon_data_.hWnd = nullptr;
}

void TrayIcon::open_settings() const {
    if (open_settings_) {
        open_settings_();
    }
}

void TrayIcon::open_debugger() const {
    if (open_debugger_) {
        open_debugger_();
    }
}

void TrayIcon::show_context_menu(POINT location) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }
    AppendMenuW(menu, MF_STRING | MF_DEFAULT, open_settings_command, L"開啟設定");
    AppendMenuW(menu, MF_STRING, open_debugger_command, L"開啟偵錯器");
    SetForegroundWindow(window_);
    const UINT command = TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, location.x, location.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (command == open_settings_command) {
        open_settings();
    } else if (command == open_debugger_command) {
        open_debugger();
    }
    Shell_NotifyIconW(NIM_SETFOCUS, &icon_data_);
}

}  // namespace llavon::service
