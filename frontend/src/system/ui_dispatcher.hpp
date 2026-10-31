#pragma once

#include <windows.h>
#include <winrt/base.h>

#include <cstdint>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace tsf {

// Runs tasks posted from any thread on the thread that created it, through a
// message-only window, so worker results re-enter TSF on its own STA thread.
class UiDispatcher final {
public:
    using Task = std::move_only_function<void()>;

    explicit UiDispatcher(HINSTANCE instance)
        : instance_(instance),
          class_name_(std::format(L"LlavonIme.Dispatcher.{}", reinterpret_cast<std::uintptr_t>(this))) {
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = &UiDispatcher::window_proc;
        window_class.hInstance = instance_;
        window_class.lpszClassName = class_name_.c_str();
        winrt::check_bool(RegisterClassW(&window_class));
        window_ = CreateWindowExW(0, class_name_.c_str(), L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                  instance_, this);
        if (!window_) {
            UnregisterClassW(class_name_.c_str(), instance_);
            winrt::throw_last_error();
        }
    }

    ~UiDispatcher() {
        close();
        DestroyWindow(window_);
        UnregisterClassW(class_name_.c_str(), instance_);
    }

    UiDispatcher(const UiDispatcher&) = delete;
    UiDispatcher& operator=(const UiDispatcher&) = delete;

    void post(Task task) {
        std::lock_guard lock(mutex_);
        if (closed_) return;
        const bool idle = tasks_.empty();
        tasks_.push_back(std::move(task));
        if (idle) PostMessageW(window_, drain_message, 0, 0);
    }

    // Drops pending tasks and rejects later ones; the producer may still be
    // shutting down when this is called.
    void close() noexcept {
        std::vector<Task> dropped;
        std::lock_guard lock(mutex_);
        closed_ = true;
        dropped.swap(tasks_);
    }

private:
    static constexpr UINT drain_message = WM_APP + 1;

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        if (message == drain_message) {
            if (auto* self = reinterpret_cast<UiDispatcher*>(GetWindowLongPtrW(window, GWLP_USERDATA))) {
                self->drain();
            }
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void drain() noexcept {
        std::vector<Task> tasks;
        {
            std::lock_guard lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) {
            try {
                task();
            } catch (...) {
            }
        }
    }

    HINSTANCE instance_ = nullptr;
    std::wstring class_name_;
    HWND window_ = nullptr;
    std::mutex mutex_;
    std::vector<Task> tasks_;
    bool closed_ = false;
};

}  // namespace tsf
