#include "service/npu_compiler_process.hpp"

#include <Windows.h>

#include <array>
#include <iostream>
#include <stdexcept>

namespace {
std::string long_output() {
    // Put a UTF-8 code point across both the read-buffer and line-size limits.
    return std::string(16383, 'x') + "編譯器 output complete";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        if (std::wstring_view(argv[1]) == L"--fatal-exit") {
            std::clog << "compiler starting\r\n" << std::flush;
            std::cerr << long_output() << "staging CHECK test diagnostic" << std::flush;
            // Exercise the same exit-status transport without invoking WER.
            ExitProcess(0xC0000409);
        }
        if (argc != 5 || std::wstring_view(argv[2]) != L"path with spaces\\" ||
            std::wstring_view(argv[3]) != L"quoted\"value" || std::wstring_view(argv[4]) != L"") return 2;
        std::cout << long_output() << '\n';
        return 0;
    }
    std::string captured;
    const auto logger = [&](std::string message) {
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
                                static_cast<int>(message.size()), nullptr, 0)) {
            throw std::runtime_error("Compiler diagnostic was split inside a UTF-8 code point");
        }
        captured += message;
    };
    try {
        std::array<wchar_t, 32768> filename{};
        const auto size = GetModuleFileNameW(nullptr, filename.data(), static_cast<DWORD>(filename.size()));
        if (!size || size == filename.size()) throw std::runtime_error("Cannot locate test executable");
        const std::filesystem::path executable(std::wstring_view(filename.data(), size));
        const std::array<std::wstring, 4> args{L"--success", L"path with spaces\\", L"quoted\"value", L""};
        llavon::service::ryzen_ai::detail::run_compiler_process(executable, args, logger);
        if (captured != long_output()) {
            throw std::runtime_error("Compiler output was not forwarded completely");
        }
        bool rejected = false;
        try {
            const std::array<std::wstring, 1> fatal{L"--fatal-exit"};
            llavon::service::ryzen_ai::detail::run_compiler_process(executable, fatal, logger);
        } catch (const std::runtime_error& error) {
            const std::string_view message(error.what());
            rejected = message.find("0xC0000409") != std::string_view::npos &&
                       message.find("staging CHECK test diagnostic") != std::string_view::npos &&
                       MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message.data(),
                                           static_cast<int>(message.size()), nullptr, 0) != 0;
        }
        if (!rejected || captured.find("staging CHECK test diagnostic") == std::string::npos ||
            captured.find("compiler starting") == std::string::npos) {
            throw std::runtime_error("Compiler failure lost its exit code or diagnostic");
        }
        std::cout << "Compiler exit isolation, argument quoting and output forwarding passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
