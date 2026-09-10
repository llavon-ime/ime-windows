#pragma once

#include <windows.h>
#include <llavon-debug/logger.hpp>
#include <winrt/base.h>

#include <cstdint>
#include <exception>
#include <format>
#include <functional>
#include <source_location>
#include <stacktrace>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace tsf {

namespace win {
struct check {
    constexpr explicit check(std::source_location location = std::source_location::current()) : location(location) {}

    std::source_location location;
};
}  // namespace win

/**
 * @brief Helper operator for checking HRESULT values and logging errors.
 * example usage: `SomeFunction() | win::check{};`
 */
inline void operator|(HRESULT hr, const win::check& checker) {
    if (FAILED(hr)) {
        const std::source_location& location = checker.location;
        const std::string file = location.file_name();
        const std::string function = location.function_name();

        const std::string message =
            std::format("HRESULT failure: {:#x} at {}:{} in {}", hr, file, location.line(), function);
        throw winrt::hresult_error(hr, winrt::to_hstring(message));
    }
}

inline void log_com_exception(llavon::debug::Logger& logger, std::string detail,
                              std::stacktrace trace) noexcept {
    try {
        logger.log([detail = std::move(detail), trace = std::move(trace)] {
            std::string message = "[ERROR] " + detail;
            for (std::size_t i = 1; i < trace.size(); ++i) {
                const auto& entry = trace[i];
                const auto description = entry.description();
                const auto file = entry.source_file();
                if (file.empty()) {
                    message += std::format("\n  #{} {}", i - 1, description);
                } else {
                    message += std::format("\n  #{} {} ({}:{})", i - 1, description,
                                           file, entry.source_line());
                }
            }
            return message;
        });
    } catch (...) {
    }
}

inline HRESULT handle_com_exception(
    llavon::debug::Logger& logger,
    std::source_location location = std::source_location::current()) noexcept {
    const auto trace = std::stacktrace::current();

    try {
        throw;
    } catch (const winrt::hresult_error& e) {
        const std::string detail = std::format(
            "COM exception at {}:{} in {}: hr={:#x}, msg={}", location.file_name(), location.line(),
            location.function_name(), static_cast<std::uint32_t>(e.code().value), winrt::to_string(e.message()));
        log_com_exception(logger, detail, trace);
        return e.code();
    } catch (const std::exception& e) {
        const std::string detail = std::format(
            "COM exception at {}:{} in {}: {}", location.file_name(), location.line(), location.function_name(),
            e.what());
        log_com_exception(logger, detail, trace);
        return E_FAIL;
    } catch (...) {
        const std::string detail = std::format(
            "COM exception at {}:{} in {}: unknown exception", location.file_name(), location.line(),
            location.function_name());
        log_com_exception(logger, detail, trace);
        return E_FAIL;
    }
}

class before_return {
public:
    before_return(std::function<void()> func) : func_(std::move(func)) {}
    ~before_return() {
        if (func_) func_();
    }

private:
    std::function<void()> func_;
};

inline std::u16string operator"" _u16(const wchar_t* s, std::size_t n) {
    return std::u16string(s, s + n);
}

inline std::wstring_view convu16(std::u16string_view text) {
    static_assert(sizeof(char16_t) == sizeof(wchar_t));
    return {reinterpret_cast<const wchar_t*>(text.data()), text.size()};
}

inline const wchar_t* convu16(const char16_t* ptr) {
    static_assert(sizeof(char16_t) == sizeof(wchar_t));
    return reinterpret_cast<const WCHAR*>(ptr);
}

}  // namespace tsf

namespace std {

template <>
struct formatter<std::u16string, wchar_t> : formatter<std::wstring_view, wchar_t> {
    auto format(const std::u16string& value, std::wformat_context& ctx) const {
        return formatter<std::wstring_view, wchar_t>::format(tsf::convu16(value), ctx);
    }
};

template <>
struct formatter<std::u16string_view, wchar_t> : formatter<std::wstring_view, wchar_t> {
    auto format(std::u16string_view value, std::wformat_context& ctx) const {
        return formatter<std::wstring_view, wchar_t>::format(tsf::convu16(value), ctx);
    }
};

}  // namespace std
