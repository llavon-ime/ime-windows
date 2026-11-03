#pragma once

#include <windows.h>
#include <winrt/base.h>

#include <functional>
#include <utility>

namespace tsf {

enum class TaskbarTheme { Light, Dark };

inline constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

inline TaskbarTheme current_taskbar_theme() noexcept {
    DWORD uses_light_theme = 0;
    DWORD size = sizeof(uses_light_theme);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, kPersonalizeKey, L"SystemUsesLightTheme",
                                        RRF_RT_REG_DWORD, nullptr, &uses_light_theme, &size);
    return status == ERROR_SUCCESS && uses_light_theme ? TaskbarTheme::Light : TaskbarTheme::Dark;
}

// Calls on_change on a thread-pool thread whenever the personalization
// settings change. Only watches the key; never writes to it.
class TaskbarThemeWatcher final {
public:
    using Callback = std::move_only_function<void()>;

    explicit TaskbarThemeWatcher(Callback on_change) : on_change_(std::move(on_change)) {
        winrt::check_win32(RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_NOTIFY, key_.put()));
        changed_.attach(winrt::check_pointer(CreateEventW(nullptr, FALSE, FALSE, nullptr)));
        wait_ = winrt::check_pointer(CreateThreadpoolWait(&TaskbarThemeWatcher::on_signaled, this, nullptr));
        try {
            arm();
        } catch (...) {
            CloseThreadpoolWait(wait_);
            throw;
        }
    }

    ~TaskbarThemeWatcher() {
        SetThreadpoolWait(wait_, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(wait_, TRUE);
        CloseThreadpoolWait(wait_);
    }

    TaskbarThemeWatcher(const TaskbarThemeWatcher&) = delete;
    TaskbarThemeWatcher& operator=(const TaskbarThemeWatcher&) = delete;

private:
    struct KeyTraits {
        using type = HKEY;
        static void close(type value) noexcept { RegCloseKey(value); }
        static constexpr type invalid() noexcept { return nullptr; }
    };

    void arm() {
        winrt::check_win32(RegNotifyChangeKeyValue(key_.get(), FALSE,
                                                   REG_NOTIFY_CHANGE_LAST_SET | REG_NOTIFY_THREAD_AGNOSTIC,
                                                   changed_.get(), TRUE));
        SetThreadpoolWait(wait_, changed_.get(), nullptr);
    }

    static void CALLBACK on_signaled(PTP_CALLBACK_INSTANCE, void* context, PTP_WAIT, TP_WAIT_RESULT) noexcept {
        auto* self = static_cast<TaskbarThemeWatcher*>(context);
        try {
            self->arm();
            self->on_change_();
        } catch (...) {
        }
    }

    Callback on_change_;
    winrt::handle_type<KeyTraits> key_;
    winrt::handle changed_;
    PTP_WAIT wait_ = nullptr;
};

}  // namespace tsf
