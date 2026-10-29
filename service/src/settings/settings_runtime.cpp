#include "settings_ui.hpp"
#include "settings_resources.h"

#include "settings_configuration.hpp"
#include "settings_menu_window.hpp"
#include "settings_window.hpp"
#include "../ui/ui_thread.hpp"
#include "../ui/xaml_resource.hpp"

#include <commctrl.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include <winrt/base.h>


extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::settings {
namespace {

constexpr wchar_t settings_command_window_class[] = L"LlavonImeSettingsUiCommandWindow";
constexpr UINT show_message = WM_APP + 1;
constexpr UINT hide_message = WM_APP + 2;
constexpr UINT show_context_menu_message = WM_APP + 4;
constexpr UINT pending_count_message = WM_APP + 5;

void report_settings_error(std::wstring_view message) noexcept {
    try {
        const std::wstring detail(message);
        OutputDebugStringW((L"[settings-ui] " + detail + L"\n").c_str());
        const DWORD path_length = GetEnvironmentVariableW(
            L"LLAVON_IME_TEST_ERROR_LOG", nullptr, 0);
        if (path_length != 0 && path_length < 32768) {
            std::wstring path(path_length, L'\0');
            const DWORD copied = GetEnvironmentVariableW(
                L"LLAVON_IME_TEST_ERROR_LOG", path.data(), path_length);
            if (copied != 0 && copied < path_length) {
                path.resize(copied);
                std::ofstream log(std::filesystem::path(path), std::ios::app);
                log << winrt::to_string(winrt::hstring(detail)) << '\n';
            }
            return;
        }
        MessageBoxW(nullptr, detail.c_str(), L"Llavon IME Settings", MB_OK | MB_ICONERROR);
    } catch (...) {
        OutputDebugStringW(L"[settings-ui] unable to report settings error\n");
    }
}

struct UiClientState {
    std::atomic<HWND> command_window{nullptr};
};
} // namespace

struct SettingsUi::State {
    State(ui::UiThread& thread, SettingsConfiguration configuration)
        : thread_(thread), thread_configuration_(std::move(configuration)) {}

    void set_pending_count(std::size_t count) noexcept {
        pending_count_.store(count, std::memory_order_release);
        has_pending_count_.store(true, std::memory_order_release);
        post(settings_client_, pending_count_message);
    }

    void show() const noexcept {
        post(settings_client_, show_message);
    }

    void hide() const noexcept {
        post(settings_client_, hide_message);
    }

    void show_context_menu(std::int32_t screen_x, std::int32_t screen_y) const noexcept {
        const HWND command_window = settings_client_.command_window.load(std::memory_order_acquire);
        if (command_window) {
            PostMessageW(command_window, show_context_menu_message,
                         static_cast<WPARAM>(static_cast<std::uint32_t>(screen_x)),
                         static_cast<LPARAM>(static_cast<std::uint32_t>(screen_y)));
        }
    }

    void initialize_on_thread() {
        using namespace winrt::Microsoft::UI::Xaml;
        theme_ = ui::load_xaml_resource(IDR_SETTINGS_THEME_XAML).as<ResourceDictionary>();
        Application::Current().Resources().MergedDictionaries().Append(theme_);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);
        const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
        WNDCLASSEXW window_class{sizeof(window_class)};
        window_class.lpfnWndProc = settings_command_window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = settings_command_window_class;
        if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            winrt::throw_last_error();
        }
        settings_window_ = std::make_unique<SettingsWindow>(std::move(thread_configuration_));
        settings_menu_ = std::make_unique<SettingsMenuWindow>([this] { show(); });
        if (!settings_window_->create(instance) || !settings_menu_->initialize(instance)) {
            winrt::throw_hresult(E_FAIL);
        }
        const HWND command_window = CreateWindowExW(
            0, settings_command_window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        if (!command_window) winrt::throw_last_error();
        settings_client_.command_window.store(command_window, std::memory_order_release);
    }

    void destroy_on_thread() {
        const HWND command = settings_client_.command_window.exchange(nullptr, std::memory_order_acq_rel);
        if (command) {
            DestroyWindow(command);
            MSG message{};
            while (PeekMessageW(&message, command, 0, 0, PM_REMOVE)) {}
        }
        if (settings_menu_) settings_menu_->destroy();
        if (settings_window_) settings_window_->destroy();
        settings_menu_.reset();
        settings_window_.reset();
        if (theme_) {
            const auto dictionaries = winrt::Microsoft::UI::Xaml::Application::Current().Resources().MergedDictionaries();
            uint32_t index = 0;
            if (dictionaries.IndexOf(theme_, index)) dictionaries.RemoveAt(index);
            theme_ = nullptr;
        }
        UnregisterClassW(settings_command_window_class, reinterpret_cast<HINSTANCE>(&__ImageBase));
        UnregisterClassW(L"LlavonImeSettingsMenuWindow", reinterpret_cast<HINSTANCE>(&__ImageBase));
        UnregisterClassW(L"LlavonImeSettingsWindow", reinterpret_cast<HINSTANCE>(&__ImageBase));
    }
    static State* get_runtime(HWND window, UINT message, LPARAM lparam) noexcept {
        State* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<State*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self;
    }

    static LRESULT CALLBACK settings_command_window_proc(HWND window, UINT message,
                                                         WPARAM wparam, LPARAM lparam) {
        State* self = get_runtime(window, message, lparam);

        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        try {
            if (message == show_message) {
                if (self->settings_menu_) self->settings_menu_->hide();
                self->show_on_thread();
                return 0;
            }
            if (message == hide_message) {
                if (self->settings_menu_) self->settings_menu_->hide();
                if (self->settings_window_) {
                    self->settings_window_->hide();
                }
                return 0;
            }
            if (message == show_context_menu_message) {
                if (self->settings_menu_) {
                    const POINT anchor{
                        static_cast<LONG>(static_cast<DWORD>(wparam)),
                        static_cast<LONG>(static_cast<DWORD>(lparam)),
                    };
                    if (!self->settings_menu_->show(
                            reinterpret_cast<HINSTANCE>(&__ImageBase), anchor)) {
                        report_settings_error(L"Unable to open the settings menu.");
                    }
                }
                return 0;
            }
            if (message == pending_count_message) {
                if (self->settings_window_) {
                    self->settings_window_->set_pending_count(
                        self->pending_count_.load(std::memory_order_acquire));
                }
                return 0;
            }
        } catch (const winrt::hresult_error& error) {
            report_settings_error(error.message());
            return 0;
        } catch (...) {
            report_settings_error(L"Unable to open the settings window.");
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    void show_on_thread() {
        if (!settings_window_) {
            return;
        }
        if (!settings_window_->create(reinterpret_cast<HINSTANCE>(&__ImageBase))) {
            throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
        }
        if (has_pending_count_.load(std::memory_order_acquire)) {
            settings_window_->set_pending_count(
                pending_count_.load(std::memory_order_acquire));
        }
        settings_window_->show();
    }

    static void post(const UiClientState& state, UINT message) noexcept {
        const HWND command_window = state.command_window.load(std::memory_order_acquire);
        if (command_window) {
            PostMessageW(command_window, message, 0, 0);
        }
    }

    ui::UiThread& thread_;
    UiClientState settings_client_;
    std::unique_ptr<SettingsWindow> settings_window_;
    std::unique_ptr<SettingsMenuWindow> settings_menu_;
    winrt::Microsoft::UI::Xaml::ResourceDictionary theme_{nullptr};
    SettingsConfiguration thread_configuration_;
    std::atomic<std::size_t> pending_count_{0};
    std::atomic_bool has_pending_count_{false};
};

SettingsUi::SettingsUi(ui::UiThread& thread, SettingsConfiguration configuration)
    : state_(std::make_unique<State>(thread, std::move(configuration))) {
    try {
        thread.invoke([this] { state_->initialize_on_thread(); });
    } catch (...) {
        thread.invoke([this] { state_->destroy_on_thread(); });
        throw;
    }
}

SettingsUi::~SettingsUi() {
    state_->thread_.invoke([this] { state_->destroy_on_thread(); });
}

void SettingsUi::show() const noexcept {
    state_->show();
}
void SettingsUi::hide() const noexcept {
    state_->hide();
}
void SettingsUi::show_context_menu(POINT location) const noexcept {
    state_->show_context_menu(location.x, location.y);
}
void SettingsUi::notify_pending_count(std::size_t count) noexcept {
    state_->set_pending_count(count);
}

} // namespace llavon::settings
