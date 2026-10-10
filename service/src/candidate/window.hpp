#pragma once

#include <windows.h>

#include <cstdint>
#include <format>
#include <winrt/base.h>
#include <optional>
#include <string>
#include <string_view>

#include <ime-core/logger.hpp>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::candidate {

using llavon::ime::core::LogInformation;

class Window {
public:
    explicit Window(llavon::ime::core::Logger& logger) noexcept : logger_(&logger) {}
    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    Window(Window&& other) noexcept : logger_(other.logger_) {
        hwnd_ = other.hwnd_;
        other.hwnd_ = nullptr;
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
            return std::format("Window::Window(Window&&), this={:X}, hwnd={:X}, moved handle", object,
                               window);
        });
    }

    Window& operator=(Window&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        destroy();
        logger_ = other.logger_;
        hwnd_ = other.hwnd_;
        other.hwnd_ = nullptr;
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
            return std::format("Window::operator=(Window&&), this={:X}, hwnd={:X}, moved handle", object,
                               window);
        });
        return *this;
    }

    virtual ~Window() { destroy(); }

    [[nodiscard]] HWND hwnd() const noexcept { return hwnd_; }

    [[nodiscard]] bool created() const noexcept { return hwnd_ != nullptr; }

    bool create(DWORD exStyle, DWORD style, std::wstring_view title, int x, int y, int width, int height,
                HWND parent = nullptr, HMENU menu = nullptr) {
        return create_impl(std::nullopt, exStyle, style, title, x, y, width, height, parent, menu);
    }

    bool create_in_band_or_fallback(DWORD band, DWORD exStyle, DWORD style, std::wstring_view title, int x,
                                    int y, int width, int height, HWND parent = nullptr,
                                    HMENU menu = nullptr) {
        return create_impl(band, exStyle, style, title, x, y, width, height, parent, menu);
    }

    void destroy() noexcept {
        if (!hwnd_) {
            return;
        }
        const HWND toDestroy = hwnd_;
        hwnd_ = nullptr;
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                             target = reinterpret_cast<std::uintptr_t>(toDestroy)] {
            return std::format("Window::destroy, this={:X}, hwnd={:X}, DestroyWindow target={:X}", object,
                               window, target);
        });
        const BOOL ok = DestroyWindow(toDestroy);
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_), ok = (ok != 0),
                                             error = GetLastError()] {
            return std::format(
                "Window::destroy, this={:X}, hwnd={:X}, DestroyWindow result={}, GetLastError={}", object,
                window, ok, error);
        });
    }

    void show(int command = SW_SHOWNOACTIVATE) const noexcept {
        if (!hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::show, this={:X}, hwnd={:X}, ignored: hwnd is null", object,
                                   window);
            });
            return;
        }
        logger_->log(LogInformation::debug,
                     [object = reinterpret_cast<std::uintptr_t>(this),
                      window = reinterpret_cast<std::uintptr_t>(hwnd_), command = command] {
                         return std::format("Window::show, this={:X}, hwnd={:X}, ShowWindow command={}",
                                            object, window, command);
                     });
        ShowWindow(hwnd_, command);
    }

    void hide() const noexcept {
        if (!hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::hide, this={:X}, hwnd={:X}, ignored: hwnd is null", object,
                                   window);
            });
            return;
        }
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
            return std::format("Window::hide, this={:X}, hwnd={:X}, ShowWindow SW_HIDE", object, window);
        });
        ShowWindow(hwnd_, SW_HIDE);
    }

    void invalidate(BOOL erase = FALSE) const noexcept {
        if (!hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::invalidate, this={:X}, hwnd={:X}, ignored: hwnd is null", object,
                                   window);
            });
            return;
        }
        const BOOL ok = InvalidateRect(hwnd_, nullptr, erase);
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                             erase = (erase != 0), ok = (ok != 0), error = GetLastError()] {
            return std::format(
                "Window::invalidate, this={:X}, hwnd={:X}, InvalidateRect erase={}, ok={}, GetLastError={}",
                object, window, erase, ok, error);
        });
    }

    void set_window_pos(HWND insertAfter, int x, int y, int width, int height, UINT flags) const noexcept {
        if (!hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::set_window_pos, this={:X}, hwnd={:X}, ignored: hwnd is null",
                                   object, window);
            });
            return;
        }
        const BOOL ok = SetWindowPos(hwnd_, insertAfter, x, y, width, height, flags);
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                             insert_after = reinterpret_cast<std::uintptr_t>(insertAfter),
                                             x = x, y = y, width = width, height = height, flags = flags,
                                             ok = (ok != 0), error = GetLastError()] {
            return std::format("Window::set_window_pos, this={:X}, hwnd={:X}, insertAfter={:X}, x={}, y={}, "
                               "w={}, h={}, flags={}, ok={}, GetLastError={}",
                               object, window, insert_after, x, y, width, height, flags, ok, error);
        });
    }

    void move(int x, int y, int width, int height, bool repaint = true) const noexcept {
        if (!hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::move, this={:X}, hwnd={:X}, ignored: hwnd is null", object,
                                   window);
            });
            return;
        }
        const BOOL ok = MoveWindow(hwnd_, x, y, width, height, repaint ? TRUE : FALSE);
        logger_->log(LogInformation::debug,
                     [object = reinterpret_cast<std::uintptr_t>(this),
                      window = reinterpret_cast<std::uintptr_t>(hwnd_), x = x, y = y, width = width,
                      height = height, repaint = (repaint != 0), ok = (ok != 0), error = GetLastError()] {
                         return std::format("Window::move, this={:X}, hwnd={:X}, x={}, y={}, w={}, h={}, "
                                            "repaint={}, ok={}, GetLastError={}",
                                            object, window, x, y, width, height, repaint, ok, error);
                     });
    }

protected:
    // The owner keeps the injected logger alive until the window is destroyed.
    llavon::ime::core::Logger* logger_;

    virtual const wchar_t* class_name() const noexcept = 0;

    virtual DWORD class_style() const noexcept { return CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS; }

    virtual HBRUSH class_background() const noexcept { return nullptr; }

    virtual HCURSOR class_cursor() const noexcept { return LoadCursorW(nullptr, IDC_ARROW); }

    virtual HICON class_icon() const noexcept { return nullptr; }

    virtual LRESULT handle_message(UINT message, WPARAM wParam, LPARAM lParam) {
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }

    virtual void on_final_destroy() noexcept {}

private:
    using CreateWindowInBandFn = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND,
                                               HMENU, HINSTANCE, void*, DWORD);
    using GetWindowBandFn = BOOL(WINAPI*)(HWND, DWORD*);

    bool create_impl(std::optional<DWORD> preferred_band, DWORD exStyle, DWORD style, std::wstring_view title,
                     int x, int y, int width, int height, HWND parent, HMENU menu) {
        logger_->log(LogInformation::debug,
                     [object = reinterpret_cast<std::uintptr_t>(this),
                      window = reinterpret_cast<std::uintptr_t>(hwnd_), ex_style = exStyle, style = style,
                      x = x, y = y, width = width, height = height,
                      parent = reinterpret_cast<std::uintptr_t>(parent), band = preferred_band] {
                         return std::format("Window::create, this={:X}, hwnd={:X}, request exStyle={}, "
                                            "style={}, x={}, y={}, w={}, h={}, parent={:X}, preferredBand={}",
                                            object, window, ex_style, style, x, y, width, height, parent,
                                            (band ? std::to_string(*band) : "none"));
                     });
        if (hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::create, this={:X}, hwnd={:X}, already created", object, window);
            });
            return true;
        }
        if (!register_class()) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                return std::format("Window::create, this={:X}, hwnd={:X}, register_class failed", object,
                                   window);
            });
            return false;
        }

        if (preferred_band) {
            // CreateWindowInBand is exported by user32.dll but is not a documented Win32 API and has no
            // supported SDK declaration. Resolve it dynamically so Windows versions that remove or restrict
            // it still use the ordinary CreateWindowExW path below.
            const HMODULE user32 = GetModuleHandleW(L"user32.dll");
            const auto create_window_in_band =
                user32 ? reinterpret_cast<CreateWindowInBandFn>(GetProcAddress(user32, "CreateWindowInBand"))
                       : nullptr;
            if (create_window_in_band) {
                SetLastError(ERROR_SUCCESS);
                hwnd_ = create_window_in_band(exStyle, class_name(), title.empty() ? nullptr : title.data(),
                                              style, x, y, width, height, parent, menu, module_instance(),
                                              this, *preferred_band);
                const DWORD band_error = hwnd_ ? ERROR_SUCCESS : GetLastError();
                if (hwnd_) {
                    DWORD actual_band = 0;
                    const auto get_window_band =
                        reinterpret_cast<GetWindowBandFn>(GetProcAddress(user32, "GetWindowBand"));
                    const BOOL got_band = get_window_band ? get_window_band(hwnd_, &actual_band) : FALSE;
                    logger_->log(
                        LogInformation::debug,
                        [object = reinterpret_cast<std::uintptr_t>(this),
                         window = reinterpret_cast<std::uintptr_t>(hwnd_), requested_band = *preferred_band,
                         band = got_band ? std::optional<DWORD>(actual_band) : std::nullopt] {
                            return std::format("Window::create, this={:X}, hwnd={:X}, CreateWindowInBand "
                                               "success, requestedBand={}, actualBand={}",
                                               object, window, requested_band,
                                               (band ? std::to_string(*band) : "unavailable"));
                        });
                    if (!got_band || actual_band == *preferred_band) {
                        return true;
                    }
                    logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                         window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                        return std::format(
                            "Window::create, this={:X}, hwnd={:X}, CreateWindowInBand returned an unexpected "
                            "band; destroying it and falling back to CreateWindowExW",
                            object, window);
                    });
                    destroy();
                } else {
                    if (band_error == ERROR_ACCESS_DENIED) {
                        logger_->log(
                            LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                    window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                                return std::format(
                                    "Window::create, this={:X}, hwnd={:X}, CreateWindowInBand denied with "
                                    "ERROR_ACCESS_DENIED; falling back to CreateWindowExW",
                                    object, window);
                            });
                    } else {
                        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                             window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                                             error = band_error] {
                            return std::format("Window::create, this={:X}, hwnd={:X}, CreateWindowInBand "
                                               "failed, GetLastError={}; falling back to CreateWindowExW",
                                               object, window, error);
                        });
                    }
                }
            } else {
                logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                     window = reinterpret_cast<std::uintptr_t>(hwnd_)] {
                    return std::format("Window::create, this={:X}, hwnd={:X}, CreateWindowInBand "
                                       "unavailable; falling back to CreateWindowExW",
                                       object, window);
                });
            }
        }

        hwnd_ = CreateWindowExW(exStyle, class_name(), title.empty() ? nullptr : title.data(), style, x, y,
                                width, height, parent, menu, module_instance(), this);

        if (!hwnd_) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                                 error = GetLastError()] {
                return std::format(
                    "Window::create, this={:X}, hwnd={:X}, CreateWindowExW failed, GetLastError={}", object,
                    window, error);
            });
            return false;
        }
        logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                             window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                             message = preferred_band ? "CreateWindowExW fallback success"
                                                                      : "CreateWindowExW success"] {
            return std::format("Window::create, this={:X}, hwnd={:X}, {}", object, window, message);
        });
        return true;
    }
    [[nodiscard]] bool register_class() const noexcept {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);

        if (GetClassInfoExW(module_instance(), class_name(), &wc) != FALSE) {
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                                 name = std::wstring(class_name())] {
                return std::format(
                    "Window::register_class, this={:X}, hwnd={:X}, class already registered: {}", object,
                    window, winrt::to_string(name));
            });
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
            logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(this),
                                                 window = reinterpret_cast<std::uintptr_t>(hwnd_),
                                                 name = std::wstring(class_name()), error = GetLastError()] {
                return std::format("Window::register_class, this={:X}, hwnd={:X}, RegisterClassExW failed "
                                   "for {}, GetLastError={}",
                                   object, window, winrt::to_string(name), error);
            });
            return false;
        }
        logger_->log(LogInformation::debug,
                     [object = reinterpret_cast<std::uintptr_t>(this),
                      window = reinterpret_cast<std::uintptr_t>(hwnd_), name = std::wstring(class_name())] {
                         return std::format(
                             "Window::register_class, this={:X}, hwnd={:X}, RegisterClassExW success for {}",
                             object, window, winrt::to_string(name));
                     });
        return true;
    }

    static HINSTANCE module_instance() noexcept { return reinterpret_cast<HINSTANCE>(&__ImageBase); }

    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Window* self = nullptr;

        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            self = static_cast<Window*>(create->lpCreateParams);
            if (!self) {
                return FALSE;
            }
            self->hwnd_ = hwnd;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(self),
                                                       window =
                                                           reinterpret_cast<std::uintptr_t>(self->hwnd_)] {
                return std::format("Window::window_proc, this={:X}, hwnd={:X}, WM_NCCREATE", object, window);
            });
        } else {
            self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        if (!self) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        if (message == WM_SHOWWINDOW || message == WM_WINDOWPOSCHANGED || message == WM_ACTIVATE ||
            message == WM_SIZE || message == WM_PAINT) {
            self->logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(self),
                                                       window = reinterpret_cast<std::uintptr_t>(self->hwnd_),
                                                       message = message, wparam = wParam, lparam = lParam] {
                return std::format(
                    "Window::window_proc, this={:X}, hwnd={:X}, message={}, wParam={}, lParam={}", object,
                    window, message, wparam, lparam);
            });
        }

        const LRESULT result = self->handle_message(message, wParam, lParam);
        if (message == WM_NCDESTROY) {
            self->logger_->log(LogInformation::debug, [object = reinterpret_cast<std::uintptr_t>(self),
                                                       window =
                                                           reinterpret_cast<std::uintptr_t>(self->hwnd_)] {
                return std::format("Window::window_proc, this={:X}, hwnd={:X}, WM_NCDESTROY", object, window);
            });
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            self->hwnd_ = nullptr;
            self->on_final_destroy();
        }
        return result;
    }

private:
    HWND hwnd_ = nullptr;
};

} // namespace llavon::candidate
