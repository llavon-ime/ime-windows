#include "ui_thread.hpp"
#include "winui_runtime.hpp"

#include <windows.h>
#include <objbase.h>
#include <microsoft.ui.dispatching.interop.h>
#include <winrt/base.h>
#include <atomic>
#include <cstdint>
#include <exception>
#include <utility>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::ui {
namespace {
constexpr wchar_t command_class[] = L"LlavonImeWinuiCommandWindow";
constexpr UINT invoke_message = WM_APP + 100;
constexpr UINT stop_message = WM_APP + 101;

struct Invocation {
    std::function<void()> work;
    std::exception_ptr error;
};

LRESULT invoke_callback(Invocation& invocation) noexcept {
    try {
        invocation.work();
    } catch (...) {
        invocation.error = std::current_exception();
    }
    return 0;
}
} // namespace

struct UiThread::State {
    std::int32_t start() {
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
        }
        return result;
    }

    void invoke(std::function<void()> work) const {
        const HWND window = command_.load(std::memory_order_acquire);
        if (!window) winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_STATE));
        Invocation invocation{std::move(work), {}};
        if (GetCurrentThreadId() == thread_id_) {
            invoke_callback(invocation);
        } else {
            SendMessageW(window, invoke_message, 0, reinterpret_cast<LPARAM>(&invocation));
        }
        if (invocation.error) std::rethrow_exception(invocation.error);
    }

    std::int32_t shutdown() {
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
        release_process_resources();
        return 0;
    }

private:
    void release_process_resources() noexcept {
        if (mta_cookie_) {
            CoDecrementMTAUsage(mta_cookie_);
            mta_cookie_ = nullptr;
        }
    }

    static DWORD WINAPI thread_entry(void* context) noexcept {
        return static_cast<State*>(context)->run();
    }

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
            return invoke_callback(*reinterpret_cast<Invocation*>(lparam));
        }
        if (message == stop_message) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    HANDLE thread_ = nullptr;
    HANDLE ready_ = nullptr;
    DWORD thread_id_ = 0;
    CO_MTA_USAGE_COOKIE mta_cookie_ = nullptr;
    std::atomic<HWND> command_{nullptr};
    std::atomic<std::int32_t> start_result_{E_FAIL};
};

UiThread::UiThread() : state_(std::make_unique<State>()) {
    const auto result = state_->start();
    if (result != 0) winrt::throw_hresult(result < 0 ? result : HRESULT_FROM_WIN32(result));
}

UiThread::~UiThread() {
    if (state_->shutdown() != 0) {
        // The thread still owns WinUI state. Destroying it would be a use-after-free.
        OutputDebugStringW(
            L"[winui] UI thread did not stop; retaining its state until process exit\n");
        (void)state_.release();
    }
}

void UiThread::invoke(std::function<void()> work) const {
    state_->invoke(std::move(work));
}

} // namespace llavon::ui
