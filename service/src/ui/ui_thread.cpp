#include "ui_thread.hpp"
#include "winui_runtime.hpp"

#include <windows.h>
#include <objbase.h>
#include <microsoft.ui.dispatching.interop.h>
#include <winrt/base.h>
#include <atomic>
#include <mutex>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::ui {
namespace {
constexpr wchar_t command_class[] = L"LlavonImeWinuiCommandWindow";
constexpr UINT invoke_message = WM_APP + 100;
constexpr UINT stop_message = WM_APP + 101;

struct Invocation {
    void (*callback)(void*);
    void* context;
};

std::int32_t invoke_callback(const Invocation& invocation) noexcept {
    try {
        invocation.callback(invocation.context);
        return 0;
    } catch (const winrt::hresult_error& error) {
        OutputDebugStringW(error.message().c_str());
        return error.code().value;
    } catch (...) {
        return E_FAIL;
    }
}

class UiThread final {
public:
    std::int32_t acquire() {
        std::lock_guard lock(mutex_);
        if (terminal_)
            return ERROR_SHUTDOWN_IN_PROGRESS;
        if (thread_) {
            ++clients_;
            return 0;
        }
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&thread_entry),
                                &module_reference_)) {
            return static_cast<std::int32_t>(GetLastError());
        }
        if (!mta_cookie_) {
            const HRESULT apartment_result = CoIncrementMTAUsage(&mta_cookie_);
            if (FAILED(apartment_result)) {
                release_process_resources();
                return apartment_result;
            }
        }
        ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready_) {
            const auto result = static_cast<std::int32_t>(GetLastError());
            release_process_resources();
            return result;
        }
        thread_ = CreateThread(nullptr, 0, thread_entry, this, 0, &thread_id_);
        if (!thread_) {
            const auto result = static_cast<std::int32_t>(GetLastError());
            CloseHandle(ready_);
            ready_ = nullptr;
            release_process_resources();
            return result;
        }
        WaitForSingleObject(ready_, INFINITE);
        CloseHandle(ready_);
        ready_ = nullptr;
        const auto result = start_result_.load(std::memory_order_acquire);
        if (result != 0) {
            WaitForSingleObject(thread_, INFINITE);
            CloseHandle(thread_);
            thread_ = nullptr;
            release_process_resources();
        } else {
            clients_ = 1;
        }
        return result;
    }

    std::int32_t invoke(void (*callback)(void*), void* context) const {
        if (!callback)
            return ERROR_INVALID_PARAMETER;
        const HWND window = command_.load(std::memory_order_acquire);
        if (!window)
            return ERROR_INVALID_STATE;
        const Invocation invocation{callback, context};
        if (GetCurrentThreadId() == GetWindowThreadProcessId(window, nullptr)) {
            return invoke_callback(invocation);
        }
        // The acquired client keeps this thread alive until the callback returns.
        return static_cast<std::int32_t>(
            SendMessageW(window, invoke_message, 0, reinterpret_cast<LPARAM>(&invocation)));
    }

    std::int32_t release() {
        std::lock_guard lock(mutex_);
        if (clients_ != 0)
            --clients_;
        return 0;
    }

    std::int32_t shutdown() {
        std::lock_guard lock(mutex_);
        if (clients_ != 0)
            return ERROR_BUSY;
        if (!thread_)
            return 0;
        if (GetCurrentThreadId() == thread_id_)
            return ERROR_INVALID_STATE;
        if (!PostMessageW(command_.load(std::memory_order_acquire), stop_message, 0, 0)) {
            return static_cast<std::int32_t>(GetLastError());
        }
        const DWORD result = WaitForSingleObject(thread_, 10000);
        if (result != WAIT_OBJECT_0) {
            return result == WAIT_TIMEOUT ? ERROR_TIMEOUT : static_cast<std::int32_t>(GetLastError());
        }
        CloseHandle(thread_);
        thread_ = nullptr;
        thread_id_ = 0;
        clients_ = 0;
        terminal_ = true;
        release_process_resources();
        return 0;
    }

private:
    void release_process_resources() noexcept {
        if (mta_cookie_) {
            CoDecrementMTAUsage(mta_cookie_);
            mta_cookie_ = nullptr;
        }
        if (module_reference_) {
            FreeLibrary(module_reference_);
            module_reference_ = nullptr;
        }
    }

    static DWORD WINAPI thread_entry(void* context) noexcept { return static_cast<UiThread*>(context)->run(); }

    DWORD run() noexcept {
        bool apartment = false;
        bool ready_signaled = false;
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment = true;
            WinuiRuntime runtime;
            const auto instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            WNDCLASSEXW window_class{sizeof(window_class)};
            window_class.lpfnWndProc = window_proc;
            window_class.hInstance = instance;
            window_class.lpszClassName = command_class;
            if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                winrt::throw_last_error();
            }
            const HWND window =
                CreateWindowExW(0, command_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
            if (!window)
                winrt::throw_last_error();
            command_.store(window, std::memory_order_release);
            start_result_.store(0, std::memory_order_release);
            SetEvent(ready_);
            ready_signaled = true;
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (ContentPreTranslateMessage(&message))
                    continue;
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            command_.store(nullptr, std::memory_order_release);
            DestroyWindow(window);
            UnregisterClassW(command_class, instance);
            // Only the last client can reach this point. All islands are closed,
            // so pending detach/layout work can finish before XAML shuts down.
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW(error.message().c_str());
            start_result_.store(error.code().value, std::memory_order_release);
        } catch (...) {
            start_result_.store(E_FAIL, std::memory_order_release);
        }
        command_.store(nullptr, std::memory_order_release);
        if (!ready_signaled)
            SetEvent(ready_);
        winrt::clear_factory_cache();
        if (apartment)
            winrt::uninit_apartment();
        return 0;
    }

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == invoke_message) {
            return invoke_callback(*reinterpret_cast<const Invocation*>(lparam));
        }
        if (message == stop_message) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    std::mutex mutex_;
    std::size_t clients_ = 0;
    HANDLE thread_ = nullptr;
    HANDLE ready_ = nullptr;
    DWORD thread_id_ = 0;
    CO_MTA_USAGE_COOKIE mta_cookie_ = nullptr;
    HMODULE module_reference_ = nullptr;
    bool terminal_ = false;
    std::atomic<HWND> command_{nullptr};
    std::atomic<std::int32_t> start_result_{E_FAIL};
};

UiThread& ui_thread() {
    static UiThread instance;
    return instance;
}
} // namespace
} // namespace llavon::ui

extern "C" std::int32_t llavon_ui_thread_acquire() { return llavon::ui::ui_thread().acquire(); }
extern "C" std::int32_t llavon_ui_thread_invoke(void (*callback)(void*), void* context) {
    return llavon::ui::ui_thread().invoke(callback, context);
}
extern "C" std::int32_t llavon_ui_thread_release() { return llavon::ui::ui_thread().release(); }
extern "C" std::int32_t llavon_ui_thread_shutdown() { return llavon::ui::ui_thread().shutdown(); }
