#pragma once

#include <windows.h>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

#include "candidate_log.hpp"

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::candidate {

class Window {
public:
    Window() = default;
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept {
        hwnd_ = other.hwnd_;
        other.hwnd_ = nullptr;
        log_state(L"Window(Window&&)", L"moved handle");
    }

    Window& operator=(Window&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        destroy();
        hwnd_ = other.hwnd_;
        other.hwnd_ = nullptr;
        log_state(L"operator=(Window&&)", L"moved handle");
        return *this;
    }

    virtual ~Window() {
        destroy();
    }

    [[nodiscard]] HWND hwnd() const noexcept {
        return hwnd_;
    }

    [[nodiscard]] bool created() const noexcept {
        return hwnd_ != nullptr;
    }

    bool create(DWORD exStyle, DWORD style, std::wstring_view title, int x, int y, int width, int height,
                HWND parent = nullptr, HMENU menu = nullptr) {
        return create_impl(std::nullopt, exStyle, style, title, x, y, width, height, parent, menu);
    }

    bool create_in_band_or_fallback(DWORD band, DWORD exStyle, DWORD style, std::wstring_view title, int x, int y,
                                    int width, int height, HWND parent = nullptr, HMENU menu = nullptr) {
        return create_impl(band, exStyle, style, title, x, y, width, height, parent, menu);
    }

    void destroy() noexcept {
        if (!hwnd_) {
            return;
        }
        const HWND toDestroy = hwnd_;
        hwnd_ = nullptr;
        log_state(L"destroy", L"DestroyWindow target=" + ptr_to_string(toDestroy));
        const BOOL ok = DestroyWindow(toDestroy);
        log_state(L"destroy", L"DestroyWindow result=" + std::wstring(ok ? L"TRUE" : L"FALSE") + L", GetLastError=" +
                                  std::to_wstring(GetLastError()));
    }

    void show(int command = SW_SHOWNOACTIVATE) const noexcept {
        if (!hwnd_) {
            log_state(L"show", L"ignored: hwnd is null");
            return;
        }
        log_state(L"show", L"ShowWindow command=" + std::to_wstring(command));
        ShowWindow(hwnd_, command);
    }

    void hide() const noexcept {
        if (!hwnd_) {
            log_state(L"hide", L"ignored: hwnd is null");
            return;
        }
        log_state(L"hide", L"ShowWindow SW_HIDE");
        ShowWindow(hwnd_, SW_HIDE);
    }

    void invalidate(BOOL erase = FALSE) const noexcept {
        if (!hwnd_) {
            log_state(L"invalidate", L"ignored: hwnd is null");
            return;
        }
        const BOOL ok = InvalidateRect(hwnd_, nullptr, erase);
        log_state(L"invalidate",
                  L"InvalidateRect erase=" + std::wstring(erase ? L"TRUE" : L"FALSE") + L", ok=" +
                      std::wstring(ok ? L"TRUE" : L"FALSE") + L", GetLastError=" + std::to_wstring(GetLastError()));
    }

    void set_window_pos(HWND insertAfter, int x, int y, int width, int height, UINT flags) const noexcept {
        if (!hwnd_) {
            log_state(L"set_window_pos", L"ignored: hwnd is null");
            return;
        }
        const BOOL ok = SetWindowPos(hwnd_, insertAfter, x, y, width, height, flags);
        log_state(L"set_window_pos",
                  L"insertAfter=" + ptr_to_string(insertAfter) + L", x=" + std::to_wstring(x) + L", y=" +
                      std::to_wstring(y) + L", w=" + std::to_wstring(width) + L", h=" + std::to_wstring(height) +
                      L", flags=" + std::to_wstring(flags) + L", ok=" + std::wstring(ok ? L"TRUE" : L"FALSE") +
                      L", GetLastError=" + std::to_wstring(GetLastError()));
    }

    void move(int x, int y, int width, int height, bool repaint = true) const noexcept {
        if (!hwnd_) {
            log_state(L"move", L"ignored: hwnd is null");
            return;
        }
        const BOOL ok = MoveWindow(hwnd_, x, y, width, height, repaint ? TRUE : FALSE);
        log_state(
            L"move",
            L"x=" + std::to_wstring(x) + L", y=" + std::to_wstring(y) + L", w=" + std::to_wstring(width) + L", h=" +
                std::to_wstring(height) + L", repaint=" + std::wstring(repaint ? L"TRUE" : L"FALSE") + L", ok=" +
                std::wstring(ok ? L"TRUE" : L"FALSE") + L", GetLastError=" + std::to_wstring(GetLastError()));
    }

protected:
    virtual const wchar_t* class_name() const noexcept = 0;

    virtual DWORD class_style() const noexcept {
        return CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    }

    virtual HBRUSH class_background() const noexcept {
        return nullptr;
    }

    virtual HCURSOR class_cursor() const noexcept {
        return LoadCursorW(nullptr, IDC_ARROW);
    }

    virtual HICON class_icon() const noexcept {
        return nullptr;
    }

    virtual LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam) {
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    virtual void on_final_destroy() noexcept {}

private:
    using CreateWindowInBandFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU,
                                               HINSTANCE, void*, DWORD);
    using GetWindowBandFn = BOOL(WINAPI*)(HWND, DWORD*);

    bool create_impl(std::optional<DWORD> preferred_band, DWORD exStyle, DWORD style, std::wstring_view title, int x,
                     int y, int width, int height, HWND parent, HMENU menu) {
        log_state(L"create",
                  L"request exStyle=" + std::to_wstring(exStyle) + L", style=" + std::to_wstring(style) + L", x=" +
                      std::to_wstring(x) + L", y=" + std::to_wstring(y) + L", w=" + std::to_wstring(width) + L", h=" +
                      std::to_wstring(height) + L", parent=" + ptr_to_string(parent) + L", preferredBand=" +
                      (preferred_band ? std::to_wstring(*preferred_band) : L"none"));
        if (hwnd_) {
            log_state(L"create", L"already created");
            return true;
        }
        if (!register_class()) {
            log_state(L"create", L"register_class failed");
            return false;
        }

        if (preferred_band) {
            // CreateWindowInBand is exported by user32.dll but is not a documented Win32 API and has no supported
            // SDK declaration. Resolve it dynamically so Windows versions that remove or restrict it still use the
            // ordinary CreateWindowExW path below.
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            const auto create_window_in_band =
                user32 ? reinterpret_cast<CreateWindowInBandFn>(GetProcAddress(user32, "CreateWindowInBand"))
                       : nullptr;
            if (create_window_in_band) {
                SetLastError(ERROR_SUCCESS);
                hwnd_ = create_window_in_band(exStyle, class_name(), title.empty() ? nullptr : title.data(), style,
                                               x, y, width, height, parent, menu, module_instance(), this,
                                               *preferred_band);
                const DWORD band_error = hwnd_ ? ERROR_SUCCESS : GetLastError();
                if (hwnd_) {
                    DWORD actual_band = 0;
                    const auto get_window_band =
                        reinterpret_cast<GetWindowBandFn>(GetProcAddress(user32, "GetWindowBand"));
                    const BOOL got_band = get_window_band ? get_window_band(hwnd_, &actual_band) : FALSE;
                    log_state(L"create",
                              L"CreateWindowInBand success, requestedBand=" + std::to_wstring(*preferred_band) +
                                  L", actualBand=" + (got_band ? std::to_wstring(actual_band) : L"unavailable"));
                    if (!got_band || actual_band == *preferred_band) {
                        return true;
                    }
                    log_state(L"create", L"CreateWindowInBand returned an unexpected band; destroying it and "
                                               L"falling back to CreateWindowExW");
                    destroy();
                } else {
                    if (band_error == ERROR_ACCESS_DENIED) {
                        log_state(
                            L"create",
                            L"CreateWindowInBand denied with ERROR_ACCESS_DENIED; falling back to CreateWindowExW");
                    } else {
                        log_state(L"create", L"CreateWindowInBand failed, GetLastError=" +
                                                   std::to_wstring(band_error) +
                                                   L"; falling back to CreateWindowExW");
                    }
                }
            } else {
                log_state(L"create", L"CreateWindowInBand unavailable; falling back to CreateWindowExW");
            }
        }

        hwnd_ = CreateWindowExW(exStyle, class_name(), title.empty() ? nullptr : title.data(), style, x, y, width,
                                height, parent, menu, module_instance(), this);

        if (!hwnd_) {
            log_state(L"create", L"CreateWindowExW failed, GetLastError=" + std::to_wstring(GetLastError()));
            return false;
        }
        log_state(L"create", preferred_band ? L"CreateWindowExW fallback success" : L"CreateWindowExW success");
        return true;
    }
    static std::wstring ptr_to_string(const void* ptr) {
        wchar_t buf[32] = {};
        swprintf_s(buf, L"%p", ptr);
        return buf;
    }

    void log_state(const wchar_t* stage, const std::wstring& details) const {
        DebugSink::instance().send(L"UI", L"Window::" + std::wstring(stage) + L", this=" + ptr_to_string(this) +
                                              L", hwnd=" + ptr_to_string(hwnd_) + L", " + details);
    }

    [[nodiscard]] bool register_class() const noexcept {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);

        if (GetClassInfoExW(module_instance(), class_name(), &wc) != FALSE) {
            log_state(L"register_class", L"class already registered: " + std::wstring(class_name()));
            return true;
        }

        wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = class_style();
        wc.lpfnWndProc = &Window::window_proc;
        wc.hInstance = module_instance();
        wc.lpszClassName = class_name();
        wc.hCursor = class_cursor();
        wc.hbrBackground = class_background();
        wc.hIcon = class_icon();
        wc.hIconSm = class_icon();

        const ATOM atom = RegisterClassExW(&wc);
        if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            log_state(L"register_class", L"RegisterClassExW failed for " + std::wstring(class_name()) +
                                             L", GetLastError=" + std::to_wstring(GetLastError()));
            return false;
        }
        log_state(L"register_class", L"RegisterClassExW success for " + std::wstring(class_name()));
        return true;
    }

    static HINSTANCE module_instance() noexcept {
        return reinterpret_cast<HINSTANCE>(&__ImageBase);
    }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Window* self = nullptr;

        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Window*>(create->lpCreateParams);
            if (!self) {
                DebugSink::instance().send(L"UI", L"Window::window_proc WM_NCCREATE missing self");
                return FALSE;
            }
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->log_state(L"window_proc", L"WM_NCCREATE");
        } else {
            self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        if (message == WM_SHOWWINDOW || message == WM_WINDOWPOSCHANGED || message == WM_ACTIVATE ||
            message == WM_SIZE || message == WM_PAINT) {
            self->log_state(L"window_proc", L"message=" + std::to_wstring(message) + L", wParam=" +
                                                std::to_wstring(wParam) + L", lParam=" + std::to_wstring(lParam));
        }

        const LRESULT result = self->handle_message(message, wParam, lParam);
        if (message == WM_NCDESTROY) {
            self->log_state(L"window_proc", L"WM_NCDESTROY");
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            self->on_final_destroy();
        }
        return result;
    }

private:
    HWND hwnd_ = nullptr;
};

}  // namespace llavon::candidate
