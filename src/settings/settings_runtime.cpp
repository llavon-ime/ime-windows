#include "settings_ui_api.h"

#include "settings_configuration.hpp"
#include "settings_window.hpp"

#include <commctrl.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include <winrt/base.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::settings {
namespace {

constexpr wchar_t command_window_class[] = L"LlavonImeSettingsUiCommandWindow";
constexpr UINT show_message = WM_APP + 1;
constexpr UINT hide_message = WM_APP + 2;
constexpr UINT stop_message = WM_APP + 3;
constexpr DWORD shutdown_timeout_ms = 10000;

class Runtime final {
public:
    int32_t configure(const llavon_settings_inference_device* devices,
                      std::size_t device_count,
                      std::int32_t selected_backend,
                      const wchar_t* selected_device_id,
                      const llavon_settings_inference_device* active_device,
                      std::int32_t gpu_offload,
                      std::int32_t fell_back_to_cpu,
                      llavon_settings_save_inference_callback save_callback,
                      void* save_context) {
        std::lock_guard lock(mutex_);
        if (thread_) {
            return ERROR_BUSY;
        }
        if ((device_count != 0 && !devices) || !active_device || !save_callback) {
            return ERROR_INVALID_PARAMETER;
        }

        SettingsConfiguration configuration;
        configuration.selected_backend = selected_backend;
        configuration.selected_device_id = selected_device_id ? selected_device_id : L"";
        configuration.active_device = InferenceDeviceOption{
            .backend = active_device->backend,
            .device_type = active_device->device_type,
            .device_id = active_device->device_id ? active_device->device_id : L"",
            .name = active_device->name ? active_device->name : L"",
            .description = active_device->description ? active_device->description : L"",
            .memory_total = active_device->memory_total,
        };
        configuration.gpu_offload = gpu_offload != 0;
        configuration.fell_back_to_cpu = fell_back_to_cpu != 0;
        configuration.save_callback = save_callback;
        configuration.save_context = save_context;
        configuration.devices.reserve(device_count);
        for (std::size_t index = 0; index < device_count; ++index) {
            const auto& source = devices[index];
            configuration.devices.push_back(InferenceDeviceOption{
                .backend = source.backend,
                .device_type = source.device_type,
                .device_id = source.device_id ? source.device_id : L"",
                .name = source.name ? source.name : L"",
                .description = source.description ? source.description : L"",
                .memory_total = source.memory_total,
            });
        }
        configuration_ = std::move(configuration);
        return 0;
    }

    int32_t start() {
        std::lock_guard lock(mutex_);
        if (thread_) {
            return 0;
        }

        ready_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!ready_event_) {
            return static_cast<int32_t>(GetLastError());
        }

        start_result_.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
        thread_configuration_ = configuration_;
        thread_ = CreateThread(nullptr, 0, thread_entry, this, 0, nullptr);
        if (!thread_) {
            const DWORD error = GetLastError();
            CloseHandle(ready_event_);
            ready_event_ = nullptr;
            return static_cast<int32_t>(error);
        }

        const DWORD wait_result = WaitForSingleObject(ready_event_, INFINITE);
        CloseHandle(ready_event_);
        ready_event_ = nullptr;
        if (wait_result != WAIT_OBJECT_0) {
            return static_cast<int32_t>(GetLastError());
        }

        const int32_t result = start_result_.load(std::memory_order_relaxed);
        if (result != 0) {
            WaitForSingleObject(thread_, shutdown_timeout_ms);
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        return result;
    }

    void show() const noexcept {
        post(show_message);
    }

    void hide() const noexcept {
        post(hide_message);
    }

    int32_t stop() {
        std::lock_guard lock(mutex_);
        if (!thread_) {
            return 0;
        }

        const HWND command_window = command_window_.load(std::memory_order_acquire);
        if (command_window) {
            PostMessageW(command_window, stop_message, 0, 0);
        }

        const DWORD wait_result = WaitForSingleObject(thread_, shutdown_timeout_ms);
        if (wait_result != WAIT_OBJECT_0) {
            return wait_result == WAIT_TIMEOUT ? static_cast<int32_t>(ERROR_TIMEOUT)
                                               : static_cast<int32_t>(GetLastError());
        }

        CloseHandle(thread_);
        thread_ = nullptr;
        return 0;
    }

private:
    static DWORD WINAPI thread_entry(void* context) noexcept {
        return static_cast<Runtime*>(context)->thread_main();
    }

    DWORD thread_main() noexcept {
        bool apartment_initialized = false;
        DWORD exit_code = ERROR_GEN_FAILURE;
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment_initialized = true;

            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&controls);

            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            WNDCLASSEXW window_class{sizeof(window_class)};
            window_class.lpfnWndProc = command_window_proc;
            window_class.hInstance = instance;
            window_class.lpszClassName = command_window_class;
            if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            SettingsWindow settings_window(thread_configuration_);
            settings_window_ = &settings_window;
            const HWND command_window = CreateWindowExW(
                0, command_window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
            if (!command_window) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            command_window_.store(command_window, std::memory_order_release);
            start_result_.store(0, std::memory_order_relaxed);
            SetEvent(ready_event_);

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (settings_window.pretranslate(message)) {
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            settings_window.destroy();
            drain_messages();
            settings_window_ = nullptr;
            command_window_.store(nullptr, std::memory_order_release);
            exit_code = 0;
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW((L"[settings-ui] " + std::wstring(error.message()) + L"\n").c_str());
            start_result_.store(static_cast<int32_t>(error.code().value), std::memory_order_relaxed);
            if (ready_event_) {
                SetEvent(ready_event_);
            }
        } catch (...) {
            OutputDebugStringW(L"[settings-ui] unhandled UI thread error\n");
            start_result_.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
            if (ready_event_) {
                SetEvent(ready_event_);
            }
        }

        settings_window_ = nullptr;
        command_window_.store(nullptr, std::memory_order_release);
        if (apartment_initialized) {
            winrt::uninit_apartment();
        }
        return exit_code;
    }

    static LRESULT CALLBACK command_window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        Runtime* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Runtime*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Runtime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }

        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        try {
            if (message == show_message) {
                self->show_on_thread();
                return 0;
            }
            if (message == hide_message) {
                if (self->settings_window_) {
                    self->settings_window_->hide();
                }
                return 0;
            }
            if (message == stop_message) {
                if (self->settings_window_) {
                    self->settings_window_->destroy();
                }
                DestroyWindow(window);
                PostQuitMessage(0);
                return 0;
            }
        } catch (const winrt::hresult_error& error) {
            MessageBoxW(nullptr, error.message().c_str(), L"Llavon IME Settings", MB_OK | MB_ICONERROR);
            return 0;
        } catch (...) {
            MessageBoxW(nullptr, L"Unable to open the settings window.", L"Llavon IME Settings",
                        MB_OK | MB_ICONERROR);
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
        settings_window_->show();
    }

    void post(UINT message) const noexcept {
        const HWND command_window = command_window_.load(std::memory_order_acquire);
        if (command_window) {
            PostMessageW(command_window, message, 0, 0);
        }
    }

    static void drain_messages() noexcept {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    mutable std::mutex mutex_;
    HANDLE thread_ = nullptr;
    HANDLE ready_event_ = nullptr;
    std::atomic<HWND> command_window_{nullptr};
    std::atomic<int32_t> start_result_{ERROR_GEN_FAILURE};
    SettingsWindow* settings_window_ = nullptr;
    SettingsConfiguration configuration_;
    SettingsConfiguration thread_configuration_;
};

Runtime& runtime() {
    static Runtime instance;
    return instance;
}

}  // namespace
}  // namespace llavon::settings

extern "C" int32_t llavon_settings_ui_configure(
    const struct llavon_settings_inference_device* devices,
    size_t device_count,
    int32_t selected_backend,
    const wchar_t* selected_device_id,
    const struct llavon_settings_inference_device* active_device,
    int32_t gpu_offload,
    int32_t fell_back_to_cpu,
    llavon_settings_save_inference_callback save_callback,
    void* save_context) {
    return llavon::settings::runtime().configure(
        devices, device_count, selected_backend, selected_device_id, active_device,
        gpu_offload, fell_back_to_cpu, save_callback, save_context);
}

extern "C" int32_t llavon_settings_ui_start(void) {
    return llavon::settings::runtime().start();
}

extern "C" void llavon_settings_ui_show(void) {
    llavon::settings::runtime().show();
}

extern "C" void llavon_settings_ui_hide(void) {
    llavon::settings::runtime().hide();
}

extern "C" int32_t llavon_settings_ui_stop(void) {
    return llavon::settings::runtime().stop();
}
