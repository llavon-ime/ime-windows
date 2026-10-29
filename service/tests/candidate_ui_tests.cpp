#include "candidate/candidate_ui_api.h"
#include "settings/settings_ui_api.h"
#include "ui/ui_thread.hpp"

#include <chrono>
#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
#include <UIAutomation.h>
#include <wrl/client.h>

namespace {
using namespace std::chrono_literals;

void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}

template <typename Predicate> void await_ui(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(25ms);
    }
}

int32_t save_inference(void*, int32_t, const char16_t*) { return 0; }
int32_t save_model(void*, const char16_t*) { return 0; }
int32_t save_names(void*, const llavon_settings_custom_name*, size_t) { return 0; }
int32_t save_width(void*, int32_t) { return 0; }
struct PasswordTestState {
    std::atomic_int delay_ms{0};
    std::atomic<DWORD> worker_thread{0};
};
int32_t refresh_training(void* context, llavon_settings_training_item* items, size_t capacity,
                         size_t* count, const char16_t* password, int64_t) {
    if (auto* state = static_cast<PasswordTestState*>(context)) {
        if (password && *password && !items) {
            state->worker_thread.store(GetCurrentThreadId());
            std::this_thread::sleep_for(std::chrono::milliseconds(state->delay_ms.load()));
            if (std::u16string_view(password) != u"test") return ERROR_ACCESS_DENIED;
        }
        *count = 1;
        if (items && capacity != 0) {
            const bool decrypted = password && *password;
            items[0] = {u"test-1",
                decrypted ? u"這是一段解密後才顯示的較長對話內容" : u"",
                decrypted ? u"這是用來驗證解密後才建立提示元件的測試資料" : u"",
                decrypted ? u"ㄘㄜˋ ㄕˋ" : u"", 0};
        }
        return 0;
    }
    *count = 0;
    return 0;
}
int32_t delete_training(void*, const char16_t*) { return 0; }
int32_t get_history(void*, llavon_settings_lora_history_item*, size_t, size_t* count) {
    *count = 0;
    return 0;
}
int32_t get_base_models(void*, llavon_settings_lora_base_model*, size_t, size_t* count) {
    *count = 0;
    return 0;
}
int32_t start_training(void*, const char16_t* const*, size_t, const llavon_settings_lora_options*, const char16_t*) {
    return 0;
}
int32_t get_status(void* context, llavon_settings_lora_status* status) {
    *status = {};
    if (context) {
        status->model_available = 1;
        status->trainer_available = 1;
    }
    return 0;
}
int32_t model_action(void*, int32_t) { return 0; }
void cancel_training(void*) {}
int32_t protection(void* context, int32_t action, const char16_t*, size_t* result) {
    *result = context && action == LLAVON_PROTECTION_STATUS ? 3 : 0;
    return 0;
}

RECT window_rect(HWND window) {
    RECT rect{};
    require(GetWindowRect(window, &rect) != FALSE, "Cannot read candidate bounds");
    return rect;
}

HWND find_window(const wchar_t* class_name) {
    HWND window = nullptr;
    while ((window = FindWindowExW(nullptr, window, class_name, nullptr)) != nullptr) {
        DWORD process = 0;
        GetWindowThreadProcessId(window, &process);
        if (process == GetCurrentProcessId())
            return window;
    }
    return nullptr;
}
struct UiLifetime final {
    ~UiLifetime() {
        llavon_settings_ui_stop();
        llavon_candidate_ui_stop();
        llavon_ui_thread_shutdown();
    }
};

void test_password_dialog(PasswordTestState& state) {
    // UI Automation marshals the caller's activation context to the UI thread.
    // This test host must use the same self-contained WinUI manifest as the UI.
    ACTCTXW context{sizeof(context)};
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = GetModuleHandleW(L"llavon-ime-ui-runtime.dll");
    context.lpResourceName = MAKEINTRESOURCEW(2);
    const auto activation = CreateActCtxW(&context);
    require(activation != INVALID_HANDLE_VALUE, "Cannot load UI activation context");
    ULONG_PTR cookie = 0;
    require(ActivateActCtx(activation, &cookie) != FALSE, "Cannot activate UI context");
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Cannot initialize UI automation");
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(&automation))), "Cannot create UI automation");
    require(llavon_settings_ui_start() == 0, "Settings startup failed");
    llavon_settings_ui_show();
    HWND window = nullptr;
    await_ui([&] {
        window = find_window(L"LlavonImeSettingsWindow");
        return window && IsWindowVisible(window);
    }, "Settings window did not open");
    const auto find = [&](const wchar_t* value, bool by_name = false) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> root;
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        if (FAILED(automation->ElementFromHandle(window, &root))) return element;
        VARIANT property{};
        property.vt = VT_BSTR;
        property.bstrVal = SysAllocString(value);
        Microsoft::WRL::ComPtr<IUIAutomationCondition> condition;
        const auto result = automation->CreatePropertyCondition(
            by_name ? UIA_NamePropertyId : UIA_AutomationIdPropertyId, property, &condition);
        VariantClear(&property);
        if (SUCCEEDED(result)) root->FindFirst(TreeScope_Descendants, condition.Get(), &element);
        return element;
    };
    int invocation = 0;
    const auto invoke = [&](const wchar_t* value, bool by_name = false) {
        std::cout << "Invoke #" << ++invocation << '\n' << std::flush;
        Microsoft::WRL::ComPtr<IUIAutomationElement> element;
        await_ui([&] {
            element = find(value, by_name);
            if (!element) element = find(value, true);
            BOOL enabled = FALSE;
            return element && SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled;
        }, "Dialog button did not become available");
        Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> pattern;
        require(SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&pattern))),
                "Dialog button cannot be invoked");
        const auto result = pattern->Invoke();
        if (FAILED(result)) std::cout << "Invoke HRESULT: " << std::hex << result << '\n';
        require(SUCCEEDED(result), "Dialog button invocation failed");
    };
    const auto fill = [&](const wchar_t* value) {
        Microsoft::WRL::ComPtr<IUIAutomationElement> field;
        await_ui([&] {
            field = find(L"PasswordInput");
            BOOL enabled = FALSE;
            return field && SUCCEEDED(field->get_CurrentIsEnabled(&enabled)) && enabled;
        }, "Password input did not become available");
        Microsoft::WRL::ComPtr<IUIAutomationValuePattern> pattern;
        require(SUCCEEDED(field->GetCurrentPatternAs(UIA_ValuePatternId, IID_PPV_ARGS(&pattern))),
                "Password input cannot be filled");
        BSTR text = SysAllocString(value);
        const auto result = pattern->SetValue(text);
        SysFreeString(text);
        require(SUCCEEDED(result), "Cannot fill password input");
    };
    const auto wait_closed = [&] {
        await_ui([&] { return !find(L"PasswordInput"); }, "Password dialog did not close");
    };
    invoke(L"LoraButton");
    for (const int delay : {0, 1, 50, 250, 0}) {
        std::cout << "Password dialog delay=" << delay << '\n' << std::flush;
        state.delay_ms.store(delay);
        invoke(L"TrainingDataButton");
        fill(L"wrong");
        invoke(L"繼續", true);
        await_ui([&] { return static_cast<bool>(find(L"密碼錯誤或資料處理失敗，請重試。", true)); },
                 "Wrong password did not finish");
        // A failed operation must release the button event so Cancel works.
        invoke(L"取消", true);
        wait_closed();
        invoke(L"TrainingDataButton");
        fill(L"test");
        invoke(L"繼續", true);
        DWORD_PTR response = 0;
        require(SendMessageTimeoutW(window, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &response) != 0,
                "UI message loop blocked during decryption");
        wait_closed();
        require(state.worker_thread.load() != GetWindowThreadProcessId(window, nullptr),
                "Decryption ran on the UI thread");
        invoke(L"DialogPrimaryButton");
    }
    require(llavon_settings_ui_stop() == 0, "Password dialog shutdown failed");
    automation.Reset();
    CoUninitialize();
    DeactivateActCtx(0, cookie);
    ReleaseActCtx(activation);
    std::cout << "PASS: password failure, cancel, reopen, background decryption and success\n";
}

void test_hidden_startup(std::wstring_view scenario) {
    std::wstring executable(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                           static_cast<DWORD>(executable.size()));
    require(length != 0 && length < executable.size(), "Cannot locate UI test executable");
    executable.resize(length);
    std::wstring command = L"\"" + executable + L"\" " + std::wstring(scenario);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process) != FALSE,
            "Cannot launch UI test with a hidden startup state");
    const std::unique_ptr<void, decltype(&CloseHandle)> process_handle(process.hProcess, CloseHandle);
    const std::unique_ptr<void, decltype(&CloseHandle)> thread_handle(process.hThread, CloseHandle);
    const DWORD wait = WaitForSingleObject(process_handle.get(), 30000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process_handle.get(), 1);
        require(false, "Hidden-startup settings test timed out");
    }
    DWORD exit_code = 1;
    require(GetExitCodeProcess(process_handle.get(), &exit_code) != FALSE,
            "Cannot read hidden-startup settings test result");
    require(exit_code == 0, "Settings first-show test failed with STARTUPINFO SW_HIDE");
}

void capture(HWND window, const wchar_t* filename) {
    Gdiplus::GdiplusStartupInput input;
    ULONG_PTR token = 0;
    require(Gdiplus::GdiplusStartup(&token, &input, nullptr) == Gdiplus::Ok, "Cannot initialize PNG capture");
    const RECT rect = window_rect(window);
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, rect.right - rect.left, rect.bottom - rect.top);
    const auto previous = SelectObject(memory, bitmap);
    PrintWindow(window, memory, 2);
    SelectObject(memory, previous);
    Gdiplus::Status status;
    {
        Gdiplus::Bitmap image(bitmap, nullptr);
        const CLSID png{0x557cf406, 0x1a04, 0x11d3, {0x9a, 0x73, 0x00, 0x00, 0xf8, 0x1e, 0xf3, 0x2e}};
        status = image.Save(filename, &png, nullptr);
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    Gdiplus::GdiplusShutdown(token);
    require(status == Gdiplus::Ok, "Cannot save PNG capture");
}
} // namespace

int main(int argc, char** argv) {
    std::cout << "Starting WinUI integration test\n" << std::flush;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    // Prevent a settings error from opening a blocking message box in the test.
    SetEnvironmentVariableW(L"LLAVON_IME_TEST_ERROR_LOG", L"candidate-ui-test-errors.log");
    UiLifetime lifetime;
    try {
        if (argc > 1 && std::string_view(argv[1]) == "settings-window-hidden") {
            test_hidden_startup(L"settings-window-first");
            std::cout << "PASS: settings first show with hidden process startup\n";
            return 0;
        }
        if (argc > 1 && std::string_view(argv[1]) == "settings-menu-hidden") {
            test_hidden_startup(L"settings-menu-first");
            std::cout << "PASS: settings menu first show with hidden process startup\n";
            return 0;
        }
        const bool password_test = argc > 1 && std::string_view(argv[1]) == "settings-password";
        PasswordTestState password_state;
        const llavon_settings_inference_device cpu{
            LLAVON_SETTINGS_BACKEND_CPU, LLAVON_SETTINGS_DEVICE_CPU, u"cpu", u"CPU", u"CPU", 0};
        require(llavon_settings_ui_configure_v6(
                    &cpu, 1, LLAVON_SETTINGS_BACKEND_CPU, u"cpu", &cpu, 0, 0, save_inference, nullptr, u"", save_model,
                    nullptr, nullptr, 0, save_names, nullptr, 0, save_width, nullptr, nullptr, 0, refresh_training,
                    password_test ? &password_state : nullptr, delete_training, nullptr, get_history, nullptr, start_training, nullptr, get_status,
                    password_test ? &password_state : nullptr, model_action, nullptr, cancel_training, nullptr,
                    protection, password_test ? &password_state : nullptr) == 0,
                "Settings configuration failed");
        require(llavon_settings_ui_configure_immediate_conversion(0, save_width, nullptr) == 0,
                "Prediction display configuration failed");

        if (password_test) {
            require(llavon_settings_ui_configure_lora_base_models_v1(get_base_models, nullptr) == 0,
                    "Cannot configure base model catalog");
            test_password_dialog(password_state);
            return 0;
        }

        const bool settings_first = argc > 1 && std::string_view(argv[1]) == "settings-first";
        const bool menu_first = argc > 1 && std::string_view(argv[1]) == "settings-menu-first";
        if (menu_first || (argc > 1 && std::string_view(argv[1]) == "settings-window-first")) {
            require(llavon_settings_ui_start() == 0, "Settings startup failed");
            HWND menu = nullptr;
            if (menu_first) {
                MONITORINFO monitor{sizeof(monitor)};
                require(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY),
                                        &monitor) != FALSE, "Cannot locate settings menu monitor");
                llavon_settings_ui_show_context_menu(monitor.rcWork.left + 200,
                                                      monitor.rcWork.top + 200);
                await_ui([&] {
                    menu = find_window(L"LlavonImeSettingsMenuWindow");
                    return menu && IsWindowVisible(menu) && GetWindow(menu, GW_CHILD);
                }, "Settings menu did not become visible after the first show request");
            }
            llavon_settings_ui_show();
            HWND settings = nullptr;
            await_ui(
                [&] {
                    settings = find_window(L"LlavonImeSettingsWindow");
                    return settings && IsWindowVisible(settings) && GetWindow(settings, GW_CHILD);
                },
                "Settings window did not become visible after the first show request");
            if (menu_first) require(!IsWindowVisible(menu), "Opening settings did not hide its menu");
            llavon_settings_ui_hide();
            await_ui([&] { return !IsWindowVisible(settings); }, "Settings hide failed");
            llavon_settings_ui_show();
            await_ui([&] { return IsWindowVisible(settings); }, "Settings reopen failed");
            require(llavon_ui_thread_invoke([](void* context) {
                ShowWindow(static_cast<HWND>(context), SW_MINIMIZE);
            }, settings) == 0, "Cannot minimize settings");
            await_ui([&] { return IsIconic(settings); }, "Settings did not minimize");
            llavon_settings_ui_show();
            await_ui([&] { return IsWindowVisible(settings) && !IsIconic(settings); },
                     "Settings did not restore after minimizing");
            require(llavon_ui_thread_invoke([](void* context) {
                ShowWindow(static_cast<HWND>(context), SW_MAXIMIZE);
            }, settings) == 0, "Cannot maximize settings");
            llavon_settings_ui_hide();
            await_ui([&] { return !IsWindowVisible(settings); }, "Maximized settings hide failed");
            llavon_settings_ui_show();
            await_ui([&] { return IsWindowVisible(settings); }, "Maximized settings reopen failed");
            require(IsZoomed(settings) != FALSE, "Reopening settings lost its maximized state");
            require(llavon_settings_ui_stop() == 0, "Settings shutdown failed");
            require(llavon_ui_thread_shutdown() == 0, "Terminal WinUI shutdown failed");
            std::cout << "PASS: settings first show, hide, reopen, restore, maximize\n";
            return 0;
        }
        if (settings_first)
            require(llavon_settings_ui_start() == 0, "Settings startup failed");
        require(llavon_candidate_ui_start() == 0, "Candidate startup failed");
        require(llavon_candidate_ui_start() == 0, "Repeated candidate startup failed");
        if (!settings_first)
            require(llavon_settings_ui_start() == 0, "Concurrent settings startup failed");

        MONITORINFO monitor{sizeof(monitor)};
        require(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE,
                "Cannot read monitor work area");
        std::vector<std::wstring> text;
        std::vector<llavon_candidate_ui_string_view> values;
        text.reserve(36);
        for (int index = 0; index < 36; ++index)
            text.push_back(L"候選" + std::to_wstring(index + 1));
        for (const auto& value : text)
            values.push_back({value.data(), static_cast<uint32_t>(value.size())});
        llavon_candidate_ui_presentation presentation{};
        presentation.struct_size = sizeof(presentation);
        presentation.anchor_x = monitor.rcWork.left + 100;
        presentation.anchor_top = monitor.rcWork.top + 100;
        presentation.anchor_y = presentation.anchor_top + 24;
        presentation.candidate_count = 9;
        presentation.candidates = values.data();
        presentation.layout_columns = 1;
        const HWND foreground = GetForegroundWindow();
        require(llavon_candidate_ui_present(&presentation) == 0, "Single column rejected");
        HWND candidate = nullptr;
        await_ui(
            [&] {
                candidate = find_window(L"TSF_CandidatePopupWindow");
                return candidate && IsWindowVisible(candidate) && GetWindow(candidate, GW_CHILD);
            },
            "WinUI candidate island did not become visible");
        require(GetForegroundWindow() == foreground, "Candidate stole foreground focus");
        require((GetWindowLongPtrW(candidate, GWL_EXSTYLE) & WS_EX_NOACTIVATE) != 0,
                "Candidate lost its no-activate style");
        const auto single = window_rect(candidate);
        const bool capture_images = argc > 2 && std::string_view(argv[2]) == "capture";
        if (capture_images) {
            std::this_thread::sleep_for(250ms);
            capture(candidate, L"candidate-single.png");
        }

        presentation.candidate_count = 36;
        presentation.layout_columns = 4;
        presentation.number_column = 3;
        presentation.selection_index = 35;
        presentation.can_prev_page = 1;
        presentation.can_next_page = 1;
        require(llavon_candidate_ui_present(&presentation) == 0, "Four columns rejected");
        await_ui(
            [&] {
                const auto rect = window_rect(candidate);
                return rect.right - rect.left > single.right - single.left;
            },
            "XAML measurement did not expand for four columns");
        const auto expanded = window_rect(candidate);
        if (capture_images) {
            std::this_thread::sleep_for(250ms);
            capture(candidate, L"candidate-expanded.png");
        }
        require(expanded.bottom - expanded.top == single.bottom - single.top,
                "Column count changed the nine-row height");

        presentation.anchor_x = monitor.rcWork.right - 1;
        presentation.anchor_top = monitor.rcWork.bottom - 30;
        presentation.anchor_y = monitor.rcWork.bottom - 6;
        require(llavon_candidate_ui_present(&presentation) == 0, "Edge presentation rejected");
        await_ui(
            [&] {
                const auto rect = window_rect(candidate);
                return rect.right == monitor.rcWork.right && rect.bottom < presentation.anchor_top;
            },
            "Candidate did not flip above the composition at the monitor boundary");
        llavon_candidate_ui_hide();
        await_ui([&] { return !IsWindowVisible(candidate); }, "Candidate hide failed");

        llavon_settings_ui_show_context_menu(monitor.rcWork.left + 200, monitor.rcWork.top + 200);
        await_ui([] { return IsWindowVisible(find_window(L"LlavonImeSettingsMenuWindow")); },
                 "Settings XAML failed while candidate runtime was active");
        require(GetWindowThreadProcessId(candidate, nullptr) ==
                    GetWindowThreadProcessId(find_window(L"LlavonImeSettingsMenuWindow"), nullptr),
                "Settings and candidates do not share the WinUI STA");
        require(llavon_settings_ui_stop() == 0, "Concurrent settings shutdown failed");
        presentation.candidate_count = 1;
        presentation.layout_columns = 1;
        presentation.number_column = 0;
        presentation.selection_index = 0;
        require(llavon_candidate_ui_present(&presentation) == 0, "Candidate after settings shutdown rejected");
        await_ui(
            [&] {
                const auto rect = window_rect(candidate);
                return IsWindowVisible(candidate) && rect.bottom - rect.top < single.bottom - single.top;
            },
            "Candidate XAML stopped working after settings shutdown");
        require(llavon_candidate_ui_stop() == 0, "Candidate shutdown failed");
        std::cout << "WinUI shutdown completed\n" << std::flush;
        require(!IsWindow(candidate), "Candidate HWND leaked after shutdown");
        require(llavon_candidate_ui_start() == 0, "Candidate restart failed");
        require(llavon_candidate_ui_present(&presentation) == 0, "Presentation after restart rejected");
        await_ui([] { return IsWindowVisible(find_window(L"TSF_CandidatePopupWindow")); },
                 "XAML did not render after restart");
        require(llavon_candidate_ui_stop() == 0, "Candidate restart shutdown failed");
        require(llavon_ui_thread_shutdown() == 0, "Terminal WinUI shutdown failed");
        std::cout << "PASS: WinUI startup order, columns, focus, bounds, hide, "
                     "concurrent settings, restart\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
