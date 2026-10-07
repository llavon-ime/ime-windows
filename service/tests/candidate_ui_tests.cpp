#include "candidate/candidate_ui_api.h"
#include "settings/settings_ui_api.h"
#include "ui/ui_thread.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

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
int32_t refresh_training(void*, llavon_settings_training_item*, size_t, size_t* count, const char16_t*, int64_t) {
    *count = 0;
    return 0;
}
int32_t delete_training(void*, const char16_t*) { return 0; }
int32_t get_history(void*, llavon_settings_lora_history_item*, size_t, size_t* count) {
    *count = 0;
    return 0;
}
int32_t start_training(void*, const char16_t* const*, size_t, const llavon_settings_lora_options*, const char16_t*) {
    return 0;
}
int32_t get_status(void*, llavon_settings_lora_status* status) {
    *status = {};
    return 0;
}
int32_t model_action(void*, int32_t) { return 0; }
void cancel_training(void*) {}
int32_t protection(void*, int32_t, const char16_t*, size_t*) { return 0; }

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
        const llavon_settings_inference_device cpu{
            LLAVON_SETTINGS_BACKEND_CPU, LLAVON_SETTINGS_DEVICE_CPU, u"cpu", u"CPU", u"CPU", 0};
        require(llavon_settings_ui_configure_v5(
                    &cpu, 1, LLAVON_SETTINGS_BACKEND_CPU, u"cpu", &cpu, 0, 0, save_inference, nullptr, u"", save_model,
                    nullptr, nullptr, 0, save_names, nullptr, 0, save_width, nullptr, nullptr, 0, refresh_training,
                    nullptr, delete_training, nullptr, get_history, nullptr, start_training, nullptr, get_status,
                    nullptr, model_action, nullptr, cancel_training, nullptr, protection, nullptr) == 0,
                "Settings configuration failed");

        const bool settings_first = argc > 1 && std::string_view(argv[1]) == "settings-first";
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
