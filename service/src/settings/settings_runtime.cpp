#include "settings_ui_api.h"

#include "settings_configuration.hpp"
#include "settings_menu_window.hpp"
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

constexpr wchar_t settings_command_window_class[] = L"LlavonImeSettingsUiCommandWindow";
constexpr wchar_t menu_command_window_class[] = L"LlavonImeSettingsMenuCommandWindow";
constexpr UINT show_message = WM_APP + 1;
constexpr UINT hide_message = WM_APP + 2;
constexpr UINT stop_message = WM_APP + 3;
constexpr UINT show_context_menu_message = WM_APP + 4;
constexpr DWORD shutdown_timeout_ms = 10000;

struct UiThreadState {
    HANDLE thread = nullptr;
    HANDLE ready_event = nullptr;
    std::atomic<HWND> command_window{nullptr};
    std::atomic<int32_t> start_result{ERROR_GEN_FAILURE};
};

class Runtime final {
public:
    int32_t configure(const llavon_settings_inference_device* devices,
                      std::size_t device_count,
                      std::int32_t selected_backend,
                      const char16_t* selected_device_id,
                      const llavon_settings_inference_device* active_device,
                      std::int32_t gpu_offload,
                      std::int32_t fell_back_to_cpu,
                      llavon_settings_save_inference_callback save_callback,
                      void* save_context,
                      const char16_t* model_path,
                      llavon_settings_save_model_path_callback save_model_path_callback,
                      void* save_model_path_context,
                      const llavon_settings_custom_name* custom_names,
                      std::size_t custom_name_count,
                      llavon_settings_save_custom_names_callback save_custom_names_callback,
                      void* save_custom_names_context,
                      std::int32_t shift_space_width_toggle_enabled,
                      llavon_settings_save_width_toggle_callback save_width_toggle_callback,
                      void* save_width_toggle_context) {
        std::lock_guard lock(mutex_);
        if (settings_thread_.thread || menu_thread_.thread) {
            return ERROR_BUSY;
        }
        if ((device_count != 0 && !devices) || !active_device || !save_callback ||
            !model_path || !save_model_path_callback ||
            (custom_name_count != 0 && !custom_names) || !save_custom_names_callback ||
            !save_width_toggle_callback) {
            return ERROR_INVALID_PARAMETER;
        }

        SettingsConfiguration configuration;
        configuration.selected_backend = selected_backend;
        configuration.selected_device_id = selected_device_id ? selected_device_id : u"";
        configuration.active_device = InferenceDeviceOption{
            .backend = active_device->backend,
            .device_type = active_device->device_type,
            .device_id = active_device->device_id ? active_device->device_id : u"",
            .name = active_device->name ? active_device->name : u"",
            .description = active_device->description ? active_device->description : u"",
            .memory_total = active_device->memory_total,
        };
        configuration.gpu_offload = gpu_offload != 0;
        configuration.fell_back_to_cpu = fell_back_to_cpu != 0;
        configuration.save_callback = save_callback;
        configuration.save_context = save_context;
        configuration.model_path = model_path;
        configuration.save_model_path_callback = save_model_path_callback;
        configuration.save_model_path_context = save_model_path_context;
        configuration.save_custom_names_callback = save_custom_names_callback;
        configuration.save_custom_names_context = save_custom_names_context;
        configuration.shift_space_width_toggle_enabled =
            shift_space_width_toggle_enabled != 0;
        configuration.save_width_toggle_callback = save_width_toggle_callback;
        configuration.save_width_toggle_context = save_width_toggle_context;
        configuration.custom_names.reserve(custom_name_count);
        for (std::size_t index = 0; index < custom_name_count; ++index) {
            const auto& source = custom_names[index];
            if (!source.name || (source.reading_count != 0 && !source.readings)) {
                return ERROR_INVALID_PARAMETER;
            }
            CustomNameOption option;
            option.name = source.name;
            option.readings.reserve(source.reading_count);
            for (std::size_t reading = 0; reading < source.reading_count; ++reading) {
                if (!source.readings[reading]) return ERROR_INVALID_PARAMETER;
                option.readings.emplace_back(source.readings[reading]);
            }
            configuration.custom_names.push_back(std::move(option));
        }
        configuration.devices.reserve(device_count);
        for (std::size_t index = 0; index < device_count; ++index) {
            const auto& source = devices[index];
            configuration.devices.push_back(InferenceDeviceOption{
                .backend = source.backend,
                .device_type = source.device_type,
                .device_id = source.device_id ? source.device_id : u"",
                .name = source.name ? source.name : u"",
                .description = source.description ? source.description : u"",
                .memory_total = source.memory_total,
            });
        }
        configuration_ = std::move(configuration);
        return 0;
    }

    int32_t start() {
        std::lock_guard lock(mutex_);
        if (settings_thread_.thread && menu_thread_.thread) {
            return 0;
        }

        thread_configuration_ = configuration_;
        const int32_t settings_result = start_thread(settings_thread_, settings_thread_entry);
        if (settings_result != 0) {
            return settings_result;
        }

        const int32_t menu_result = start_thread(menu_thread_, menu_thread_entry);
        if (menu_result != 0) {
            stop_thread(settings_thread_);
            return menu_result;
        }
        return 0;
    }

    void show() const noexcept {
        post(menu_thread_, hide_message);
        post(settings_thread_, show_message);
    }

    void hide() const noexcept {
        post(menu_thread_, hide_message);
        post(settings_thread_, hide_message);
    }

    void show_context_menu(std::int32_t screen_x, std::int32_t screen_y) const noexcept {
        const HWND command_window = menu_thread_.command_window.load(std::memory_order_acquire);
        if (command_window) {
            PostMessageW(command_window, show_context_menu_message,
                         static_cast<WPARAM>(static_cast<std::uint32_t>(screen_x)),
                         static_cast<LPARAM>(static_cast<std::uint32_t>(screen_y)));
        }
    }

    int32_t stop() {
        std::lock_guard lock(mutex_);
        if (!settings_thread_.thread && !menu_thread_.thread) {
            return 0;
        }

        post(menu_thread_, stop_message);
        post(settings_thread_, stop_message);
        const int32_t menu_result = join_thread(menu_thread_);
        const int32_t settings_result = join_thread(settings_thread_);
        return menu_result != 0 ? menu_result : settings_result;
    }

private:
    using ThreadEntry = DWORD(WINAPI*)(void*);

    static DWORD WINAPI settings_thread_entry(void* context) noexcept {
        return static_cast<Runtime*>(context)->settings_thread_main();
    }

    static DWORD WINAPI menu_thread_entry(void* context) noexcept {
        return static_cast<Runtime*>(context)->menu_thread_main();
    }

    int32_t start_thread(UiThreadState& state, ThreadEntry entry) {
        if (state.thread) {
            return 0;
        }

        state.ready_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!state.ready_event) {
            return static_cast<int32_t>(GetLastError());
        }

        state.start_result.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
        state.thread = CreateThread(nullptr, 0, entry, this, 0, nullptr);
        if (!state.thread) {
            const DWORD error = GetLastError();
            CloseHandle(state.ready_event);
            state.ready_event = nullptr;
            return static_cast<int32_t>(error);
        }

        const DWORD wait_result = WaitForSingleObject(state.ready_event, INFINITE);
        CloseHandle(state.ready_event);
        state.ready_event = nullptr;
        if (wait_result != WAIT_OBJECT_0) {
            const DWORD error = GetLastError();
            join_thread(state);
            return static_cast<int32_t>(error);
        }

        const int32_t result = state.start_result.load(std::memory_order_relaxed);
        if (result != 0) {
            join_thread(state);
        }
        return result;
    }

    static int32_t join_thread(UiThreadState& state) noexcept {
        if (!state.thread) {
            return 0;
        }
        const DWORD wait_result = WaitForSingleObject(state.thread, shutdown_timeout_ms);
        if (wait_result != WAIT_OBJECT_0) {
            return wait_result == WAIT_TIMEOUT ? static_cast<int32_t>(ERROR_TIMEOUT)
                                               : static_cast<int32_t>(GetLastError());
        }
        CloseHandle(state.thread);
        state.thread = nullptr;
        return 0;
    }

    static void stop_thread(UiThreadState& state) noexcept {
        post(state, stop_message);
        (void)join_thread(state);
    }

    DWORD settings_thread_main() noexcept {
        bool apartment_initialized = false;
        bool ready_signaled = false;
        const HANDLE ready_event = settings_thread_.ready_event;
        DWORD exit_code = ERROR_GEN_FAILURE;
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment_initialized = true;

            INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
            InitCommonControlsEx(&controls);

            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            WNDCLASSEXW window_class{sizeof(window_class)};
            window_class.lpfnWndProc = settings_command_window_proc;
            window_class.hInstance = instance;
            window_class.lpszClassName = settings_command_window_class;
            if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            SettingsWindow settings_window(thread_configuration_);
            settings_window_ = &settings_window;
            const HWND command_window = CreateWindowExW(
                0, settings_command_window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                instance, this);
            if (!command_window) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            settings_thread_.command_window.store(command_window, std::memory_order_release);
            settings_thread_.start_result.store(0, std::memory_order_relaxed);
            SetEvent(ready_event);
            ready_signaled = true;

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
            settings_thread_.command_window.store(nullptr, std::memory_order_release);
            exit_code = 0;
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW((L"[settings-ui] " + std::wstring(error.message()) + L"\n").c_str());
            settings_thread_.start_result.store(static_cast<int32_t>(error.code().value),
                                                std::memory_order_relaxed);
            if (!ready_signaled) {
                SetEvent(ready_event);
            }
        } catch (...) {
            OutputDebugStringW(L"[settings-ui] unhandled UI thread error\n");
            settings_thread_.start_result.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
            if (!ready_signaled) {
                SetEvent(ready_event);
            }
        }

        settings_window_ = nullptr;
        settings_thread_.command_window.store(nullptr, std::memory_order_release);
        if (apartment_initialized) {
            winrt::uninit_apartment();
        }
        return exit_code;
    }

    DWORD menu_thread_main() noexcept {
        bool apartment_initialized = false;
        bool ready_signaled = false;
        const HANDLE ready_event = menu_thread_.ready_event;
        DWORD exit_code = ERROR_GEN_FAILURE;
        try {
            winrt::init_apartment(winrt::apartment_type::single_threaded);
            apartment_initialized = true;

            const HINSTANCE instance = reinterpret_cast<HINSTANCE>(&__ImageBase);
            WNDCLASSEXW window_class{sizeof(window_class)};
            window_class.lpfnWndProc = menu_command_window_proc;
            window_class.hInstance = instance;
            window_class.lpszClassName = menu_command_window_class;
            if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            SettingsMenuWindow settings_menu([this] { show(); });
            settings_menu_ = &settings_menu;
            const HWND command_window = CreateWindowExW(
                0, menu_command_window_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                instance, this);
            if (!command_window) {
                throw winrt::hresult_error(HRESULT_FROM_WIN32(GetLastError()));
            }

            menu_thread_.command_window.store(command_window, std::memory_order_release);
            menu_thread_.start_result.store(0, std::memory_order_relaxed);
            SetEvent(ready_event);
            ready_signaled = true;

            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                if (settings_menu.pretranslate(message)) {
                    continue;
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }

            settings_menu.destroy();
            drain_messages();
            settings_menu_ = nullptr;
            menu_thread_.command_window.store(nullptr, std::memory_order_release);
            exit_code = 0;
        } catch (const winrt::hresult_error& error) {
            OutputDebugStringW(
                (L"[settings-menu] " + std::wstring(error.message()) + L"\n").c_str());
            menu_thread_.start_result.store(static_cast<int32_t>(error.code().value),
                                            std::memory_order_relaxed);
            if (!ready_signaled) {
                SetEvent(ready_event);
            }
        } catch (...) {
            OutputDebugStringW(L"[settings-menu] unhandled UI thread error\n");
            menu_thread_.start_result.store(ERROR_GEN_FAILURE, std::memory_order_relaxed);
            if (!ready_signaled) {
                SetEvent(ready_event);
            }
        }

        settings_menu_ = nullptr;
        menu_thread_.command_window.store(nullptr, std::memory_order_release);
        if (apartment_initialized) {
            winrt::uninit_apartment();
        }
        return exit_code;
    }

    static Runtime* get_runtime(HWND window, UINT message, LPARAM lparam) noexcept {
        Runtime* self = nullptr;
        if (message == WM_NCCREATE) {
            const auto create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<Runtime*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        } else {
            self = reinterpret_cast<Runtime*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        }
        return self;
    }

    static LRESULT CALLBACK settings_command_window_proc(HWND window, UINT message,
                                                         WPARAM wparam, LPARAM lparam) {
        Runtime* self = get_runtime(window, message, lparam);

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

    static LRESULT CALLBACK menu_command_window_proc(HWND window, UINT message,
                                                     WPARAM wparam, LPARAM lparam) {
        Runtime* self = get_runtime(window, message, lparam);
        if (!self) {
            return DefWindowProcW(window, message, wparam, lparam);
        }

        if (message == hide_message) {
            if (self->settings_menu_) {
                self->settings_menu_->hide();
            }
            return 0;
        }
        if (message == show_context_menu_message) {
            if (self->settings_menu_) {
                const POINT anchor{
                    static_cast<LONG>(static_cast<DWORD>(wparam)),
                    static_cast<LONG>(static_cast<DWORD>(lparam)),
                };
                if (!self->settings_menu_->show(reinterpret_cast<HINSTANCE>(&__ImageBase),
                                                anchor)) {
                    OutputDebugStringW(L"[settings-menu] unable to create the XAML menu\n");
                }
            }
            return 0;
        }
        if (message == stop_message) {
            if (self->settings_menu_) {
                self->settings_menu_->destroy();
            }
            DestroyWindow(window);
            PostQuitMessage(0);
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

    static void post(const UiThreadState& state, UINT message) noexcept {
        const HWND command_window = state.command_window.load(std::memory_order_acquire);
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
    UiThreadState settings_thread_;
    UiThreadState menu_thread_;
    SettingsWindow* settings_window_ = nullptr;
    SettingsMenuWindow* settings_menu_ = nullptr;
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
    const char16_t* selected_device_id,
    const struct llavon_settings_inference_device* active_device,
    int32_t gpu_offload,
    int32_t fell_back_to_cpu,
    llavon_settings_save_inference_callback save_callback,
    void* save_context,
    const char16_t* model_path,
    llavon_settings_save_model_path_callback save_model_path_callback,
    void* save_model_path_context,
    const struct llavon_settings_custom_name* custom_names,
    size_t custom_name_count,
    llavon_settings_save_custom_names_callback save_custom_names_callback,
    void* save_custom_names_context,
    int32_t shift_space_width_toggle_enabled,
    llavon_settings_save_width_toggle_callback save_width_toggle_callback,
    void* save_width_toggle_context) {
    return llavon::settings::runtime().configure(
        devices, device_count, selected_backend, selected_device_id, active_device,
        gpu_offload, fell_back_to_cpu, save_callback, save_context,
        model_path, save_model_path_callback, save_model_path_context,
        custom_names, custom_name_count, save_custom_names_callback,
        save_custom_names_context, shift_space_width_toggle_enabled,
        save_width_toggle_callback, save_width_toggle_context);
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

extern "C" void llavon_settings_ui_show_context_menu(int32_t screen_x,
                                                       int32_t screen_y) {
    llavon::settings::runtime().show_context_menu(screen_x, screen_y);
}

extern "C" int32_t llavon_settings_ui_stop(void) {
    return llavon::settings::runtime().stop();
}
