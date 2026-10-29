#include "candidate/candidate_ui.hpp"
#include "settings/settings_ui.hpp"
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
#include <winrt/base.h>

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

struct PasswordTestState {
    std::atomic_int delay_ms{0};
    std::atomic<DWORD> worker_thread{0};
};

llavon::settings::SettingsConfiguration test_configuration(PasswordTestState* state) {
    using namespace llavon::settings;
    SettingsConfiguration config;
    const InferenceDeviceOption cpu{
        InferenceBackend::cpu, InferenceDeviceType::cpu, u"cpu", u"CPU", u"CPU", 0};
    config.devices = {cpu};
    config.active_device = cpu;
    config.selected_backend = InferenceBackend::cpu;
    config.selected_device_id = u"cpu";
    config.actions.save_inference = [](InferenceBackend, std::u16string_view) { return true; };
    config.actions.save_model_path = [](const std::filesystem::path&) { return true; };
    config.actions.save_custom_names = [](const std::vector<CustomNameOption>&) { return true; };
    config.actions.save_width_toggle = [](bool) { return true; };
    config.actions.save_immediate_conversion = [](bool) { return true; };
    config.actions.load_training_items =
        [state](std::u16string_view password,
            std::int64_t) -> std::optional<std::vector<TrainingDataOption>> {
        if (!state) return std::vector<TrainingDataOption>{};
        if (!password.empty()) {
            state->worker_thread.store(GetCurrentThreadId());
            std::this_thread::sleep_for(std::chrono::milliseconds(state->delay_ms.load()));
            if (password != u"test") return std::nullopt;
        }
        return std::vector<TrainingDataOption>{
            {u"test-1", password.empty() ? u"" : u"這是一段解密後才顯示的較長對話內容",
                password.empty() ? u"" : u"這是用來驗證解密後才建立提示元件的測試資料",
                password.empty() ? u"" : u"ㄘㄜˋ ㄕˋ", false}};
    };
    config.actions.delete_training_item = [](std::u16string_view) { return true; };
    config.actions.load_lora_history = [] {
        return std::vector<llavon::service::LoraTrainingRun>{};
    };
    config.actions.load_lora_base_models = [] {
        return std::vector<llavon::service::LoraBaseModel>{};
    };
    config.actions.start_lora_training = [](const std::vector<std::u16string>&, LoraTrainingOptions,
                                             std::u16string_view) { return true; };
    config.actions.get_lora_status = [state] {
        LoraOperationStatus status;
        status.model_available = state != nullptr;
        status.trainer_available = state != nullptr;
        return std::optional{status};
    };
    config.actions.lora_model_action = [](LoraAction) { return true; };
    config.actions.cancel_lora = [] {};
    config.actions.protection.status = [state]() -> std::optional<ProtectionStatus> {
        return ProtectionStatus{state != nullptr, state != nullptr};
    };
    config.actions.protection.setup = [](std::u16string_view) { return true; };
    config.actions.protection.set_enabled = [](bool) { return true; };
    config.actions.protection.cleanup = []() -> std::optional<std::size_t> { return 0; };
    config.actions.protection.reset = [] { return true; };
    return config;
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
    std::unique_ptr<llavon::ui::UiThread> thread;
    std::unique_ptr<llavon::candidate::CandidateUi> candidate;
    std::unique_ptr<llavon::settings::SettingsUi> settings;
};

void test_password_dialog(PasswordTestState& state, llavon::settings::SettingsUi& settings) {
    // UI Automation marshals the caller's activation context to the UI thread.
    // This test host must use the same self-contained WinUI manifest as the UI.
    ACTCTXW context{sizeof(context)};
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = GetModuleHandleW(nullptr);
    context.lpResourceName = MAKEINTRESOURCEW(1);
    const auto activation = CreateActCtxW(&context);
    require(activation != INVALID_HANDLE_VALUE, "Cannot load UI activation context");
    ULONG_PTR cookie = 0;
    require(ActivateActCtx(activation, &cookie) != FALSE, "Cannot activate UI context");
    require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "Cannot initialize UI automation");
    winrt::com_ptr<IUIAutomation> automation;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                winrt::guid_of<IUIAutomation>(), automation.put_void())),
        "Cannot create UI automation");
    settings.show();
    HWND window = nullptr;
    await_ui([&] {
        window = find_window(L"LlavonImeSettingsWindow");
        return window && IsWindowVisible(window);
    }, "Settings window did not open");
    const auto find = [&](const wchar_t* value, bool by_name = false) {
        winrt::com_ptr<IUIAutomationElement> root;
        winrt::com_ptr<IUIAutomationElement> element;
        if (FAILED(automation->ElementFromHandle(window, root.put()))) return element;
        VARIANT property{};
        property.vt = VT_BSTR;
        property.bstrVal = SysAllocString(value);
        winrt::com_ptr<IUIAutomationCondition> condition;
        const auto result = automation->CreatePropertyCondition(
            by_name ? UIA_NamePropertyId : UIA_AutomationIdPropertyId, property, condition.put());
        VariantClear(&property);
        if (SUCCEEDED(result))
            root->FindFirst(TreeScope_Descendants, condition.get(), element.put());
        return element;
    };
    int invocation = 0;
    const auto invoke = [&](const wchar_t* value, bool by_name = false) {
        std::cout << "Invoke #" << ++invocation << '\n' << std::flush;
        winrt::com_ptr<IUIAutomationElement> element;
        await_ui([&] {
            element = find(value, by_name);
            if (!element) element = find(value, true);
            BOOL enabled = FALSE;
            return element && SUCCEEDED(element->get_CurrentIsEnabled(&enabled)) && enabled;
        }, "Dialog button did not become available");
        winrt::com_ptr<IUIAutomationInvokePattern> pattern;
        require(SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId,
                    winrt::guid_of<IUIAutomationInvokePattern>(), pattern.put_void())),
            "Dialog button cannot be invoked");
        const auto result = pattern->Invoke();
        if (FAILED(result)) std::cout << "Invoke HRESULT: " << std::hex << result << '\n';
        require(SUCCEEDED(result), "Dialog button invocation failed");
    };
    const auto fill = [&](const wchar_t* value) {
        winrt::com_ptr<IUIAutomationElement> field;
        await_ui([&] {
            field = find(L"PasswordInput");
            BOOL enabled = FALSE;
            return field && SUCCEEDED(field->get_CurrentIsEnabled(&enabled)) && enabled;
        }, "Password input did not become available");
        winrt::com_ptr<IUIAutomationValuePattern> pattern;
        require(SUCCEEDED(field->GetCurrentPatternAs(UIA_ValuePatternId,
                    winrt::guid_of<decltype(pattern)::type>(), pattern.put_void())),
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
    automation = nullptr;
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
    PasswordTestState password_state;
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
        lifetime.thread = std::make_unique<llavon::ui::UiThread>();
        auto config = test_configuration(password_test ? &password_state : nullptr);
        const auto start_settings = [&] {
            if (!lifetime.settings)
                lifetime.settings =
                    std::make_unique<llavon::settings::SettingsUi>(*lifetime.thread, config);
        };
        const auto start_candidate = [&] {
            if (!lifetime.candidate)
                lifetime.candidate =
                    std::make_unique<llavon::candidate::CandidateUi>(*lifetime.thread);
        };
        if (password_test) {
            start_settings();
            test_password_dialog(password_state, *lifetime.settings);
            lifetime.settings.reset();
            return 0;
        }

        const bool settings_first = argc > 1 && std::string_view(argv[1]) == "settings-first";
        const bool menu_first = argc > 1 && std::string_view(argv[1]) == "settings-menu-first";
        if (menu_first || (argc > 1 && std::string_view(argv[1]) == "settings-window-first")) {
            start_settings();
            HWND menu = nullptr;
            if (menu_first) {
                MONITORINFO monitor{sizeof(monitor)};
                require(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY),
                                        &monitor) != FALSE, "Cannot locate settings menu monitor");
                lifetime.settings->show_context_menu(
                    {monitor.rcWork.left + 200, monitor.rcWork.top + 200});
                await_ui([&] {
                    menu = find_window(L"LlavonImeSettingsMenuWindow");
                    return menu && IsWindowVisible(menu) && GetWindow(menu, GW_CHILD);
                }, "Settings menu did not become visible after the first show request");
            }
            lifetime.settings->show();
            HWND settings = nullptr;
            await_ui(
                [&] {
                    settings = find_window(L"LlavonImeSettingsWindow");
                    return settings && IsWindowVisible(settings) && GetWindow(settings, GW_CHILD);
                },
                "Settings window did not become visible after the first show request");
            if (menu_first) require(!IsWindowVisible(menu), "Opening settings did not hide its menu");
            lifetime.settings->hide();
            await_ui([&] { return !IsWindowVisible(settings); }, "Settings hide failed");
            lifetime.settings->show();
            await_ui([&] { return IsWindowVisible(settings); }, "Settings reopen failed");
            lifetime.thread->invoke([settings] { ShowWindow(settings, SW_MINIMIZE); });
            await_ui([&] { return IsIconic(settings); }, "Settings did not minimize");
            lifetime.settings->show();
            await_ui([&] { return IsWindowVisible(settings) && !IsIconic(settings); },
                     "Settings did not restore after minimizing");
            lifetime.thread->invoke([settings] { ShowWindow(settings, SW_MAXIMIZE); });
            lifetime.settings->hide();
            await_ui([&] { return !IsWindowVisible(settings); }, "Maximized settings hide failed");
            lifetime.settings->show();
            await_ui([&] { return IsWindowVisible(settings); }, "Maximized settings reopen failed");
            require(IsZoomed(settings) != FALSE, "Reopening settings lost its maximized state");
            lifetime.settings.reset();
            lifetime.thread.reset();
            std::cout << "PASS: settings first show, hide, reopen, restore, maximize\n";
            return 0;
        }
        if (settings_first) start_settings();
        start_candidate();
        start_candidate();
        if (!settings_first) start_settings();

        require(find_window(L"TSF_CandidatePopupWindow") != nullptr,
            "Candidate HWND was not created during startup");
        require(GetWindow(find_window(L"TSF_CandidatePopupWindow"), GW_CHILD) != nullptr,
            "Candidate XAML island was not created during startup");
        require(!IsWindowVisible(find_window(L"TSF_CandidatePopupWindow")),
            "Candidate startup displayed a window");
        require(!IsWindowVisible(find_window(L"LlavonImeSettingsWindow")),
            "Settings startup displayed a window");
        MONITORINFO monitor{sizeof(monitor)};
        require(GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor) != FALSE,
                "Cannot read monitor work area");
        std::vector<std::wstring> text;
        text.reserve(36);
        for (int index = 0; index < 36; ++index)
            text.push_back(L"候選" + std::to_wstring(index + 1));
        llavon::candidate::Presentation presentation;
        presentation.anchor_x = monitor.rcWork.left + 100;
        presentation.anchor_top = monitor.rcWork.top + 100;
        presentation.anchor_y = presentation.anchor_top + 24;
        presentation.candidates.assign(text.begin(), text.begin() + 9);
        presentation.layout_columns = 1;
        const HWND foreground = GetForegroundWindow();
        require(lifetime.candidate->present(presentation), "Single column rejected");
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

        presentation.candidates.assign(text.begin(), text.begin() + 36);
        presentation.layout_columns = 4;
        presentation.number_column = 3;
        presentation.selection_index = 35;
        presentation.can_prev_page = 1;
        presentation.can_next_page = 1;
        require(lifetime.candidate->present(presentation), "Four columns rejected");
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
        require(lifetime.candidate->present(presentation), "Edge presentation rejected");
        await_ui(
            [&] {
                const auto rect = window_rect(candidate);
                return rect.right == monitor.rcWork.right && rect.bottom < presentation.anchor_top;
            },
            "Candidate did not flip above the composition at the monitor boundary");
        lifetime.candidate->hide();
        await_ui([&] { return !IsWindowVisible(candidate); }, "Candidate hide failed");

        lifetime.settings->show_context_menu({monitor.rcWork.left + 200, monitor.rcWork.top + 200});
        await_ui([] { return IsWindowVisible(find_window(L"LlavonImeSettingsMenuWindow")); },
                 "Settings XAML failed while candidate runtime was active");
        require(GetWindowThreadProcessId(candidate, nullptr) ==
                    GetWindowThreadProcessId(find_window(L"LlavonImeSettingsMenuWindow"), nullptr),
                "Settings and candidates do not share the WinUI STA");
        lifetime.settings.reset();
        presentation.candidates.assign(text.begin(), text.begin() + 1);
        presentation.layout_columns = 1;
        presentation.number_column = 0;
        presentation.selection_index = 0;
        require(lifetime.candidate->present(presentation),
            "Candidate after settings shutdown rejected");
        await_ui(
            [&] {
                const auto rect = window_rect(candidate);
                return IsWindowVisible(candidate) && rect.bottom - rect.top < single.bottom - single.top;
            },
            "Candidate XAML stopped working after settings shutdown");
        lifetime.candidate.reset();
        std::cout << "WinUI shutdown completed\n" << std::flush;
        require(!IsWindow(candidate), "Candidate HWND leaked after shutdown");
        start_candidate();
        require(lifetime.candidate->present(presentation), "Presentation after restart rejected");
        await_ui([] { return IsWindowVisible(find_window(L"TSF_CandidatePopupWindow")); },
                 "XAML did not render after restart");
        lifetime.candidate.reset();
        lifetime.thread.reset();
        std::cout << "PASS: WinUI startup order, columns, focus, bounds, hide, "
                     "concurrent settings, restart\n";
        return 0;
    } catch (const winrt::hresult_error& error) {
        std::cerr << "WinUI error: " << winrt::to_string(error.message()) << '\n';
        return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
