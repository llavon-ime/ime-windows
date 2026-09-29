#include <windows.h>
#include <objbase.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

#ifdef LLAVON_IME_RYZENAI
#include <ime-core/ryzen_ai.hpp>

#include <array>
#include <memory>

class TestLogger final : public llavon::ime::core::Logger {
public:
    void log(std::string message) noexcept override { std::cerr << message << '\n'; }
    void log(MessageFactory) noexcept override {}
};
#endif

int wmain(int argc, wchar_t** argv) {
    if (argc < 2 || argc > 4 ||
        (argc == 3 && std::wstring_view(argv[2]) != L"--default-dll-directories")) {
        std::cerr << "Usage: settings-ui-smoke-tests SETTINGS_DLL "
                     "[--default-dll-directories | NPU_MODEL NPU_CACHE]\n";
        return 1;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    const auto error_path = std::filesystem::temp_directory_path() /
        (L"llavon-settings-smoke-" + std::to_wstring(GetCurrentProcessId()) + L".log");
    std::ofstream(error_path, std::ios::trunc).close();
    if (!SetEnvironmentVariableW(L"LLAVON_IME_TEST_ERROR_LOG", error_path.c_str())) return 1;
    // Reproduce VitisAI's process-wide loader policy without needing an NPU in CI.
    if (argc == 3 && !SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) return 1;
#ifdef LLAVON_IME_RYZENAI
    std::shared_ptr<llavon::ime::core::InferenceAccelerator> accelerator;
    if (argc == 4) {
        accelerator = llavon::ime::core::create_ryzen_ai_accelerator({
            .model_directory = std::filesystem::absolute(argv[2]),
            .cache_directory = std::filesystem::absolute(argv[3]),
            .allow_compilation = false, .logger = std::make_shared<TestLogger>()});
        accelerator->prepare({}, 384);
        auto context = accelerator->create_context();
        context->decode(std::array<std::int32_t, 1>{1}, 0);
    }
#else
    if (argc == 4) {
        std::cerr << "This build does not include NPU support\n";
        return 1;
    }
#endif
    const auto module = LoadLibraryExW(argv[1], nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!module) {
        std::cerr << "Cannot load settings DLL: " << GetLastError() << '\n';
        return 1;
    }
    const auto start = reinterpret_cast<std::int32_t (*)()>(
        GetProcAddress(module, "llavon_settings_ui_start"));
    const auto stop = reinterpret_cast<std::int32_t (*)()>(
        GetProcAddress(module, "llavon_settings_ui_stop"));
    const auto show = reinterpret_cast<void (*)()>(
        GetProcAddress(module, "llavon_settings_ui_show"));
    if (!start || !stop || !show) return 1;
    const auto result = start();
    // Show and stop are queued on the same STA in order, so this also builds
    // the actual page and XAML island before checking asynchronous UI errors.
    if (result == 0) show();
    const auto stopped = stop();
    std::ifstream log(error_path);
    if (result != 0 || stopped != 0 || log.peek() != std::ifstream::traits_type::eof()) {
        std::cerr << "Settings startup=" << result << " shutdown=" << stopped << '\n';
        std::cerr << log.rdbuf();
        return 1;
    }
    log.close();
    std::filesystem::remove(error_path);
    std::cout << "Settings XAML runtime, page, control resources and shutdown passed\n";
    // WinUI owns process-wide activation factories; leave the module loaded.
    return 0;
}
