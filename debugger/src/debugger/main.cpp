#include "debugger_window.hpp"

#include <commctrl.h>
#include <windows.h>

#include <winrt/base.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&controls);

        llavon::debugger::DebuggerWindow window;
        if (!window.create(instance)) return static_cast<int>(GetLastError());
        window.show();

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (window.pretranslate(message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        window.destroy();
        winrt::uninit_apartment();
        return static_cast<int>(message.wParam);
    } catch (const winrt::hresult_error& error) {
        MessageBoxW(nullptr, error.message().c_str(), L"Llavon IME Debugger", MB_OK | MB_ICONERROR);
    } catch (...) {
        MessageBoxW(nullptr, L"Unable to start the debugger.", L"Llavon IME Debugger",
                    MB_OK | MB_ICONERROR);
    }
    return 1;
}
