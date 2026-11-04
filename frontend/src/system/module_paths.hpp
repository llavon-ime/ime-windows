#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>

namespace tsf::paths {

inline std::optional<std::filesystem::path> module_directory(HINSTANCE instance) {
    if (!instance) return std::nullopt;

    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD copied = GetModuleFileNameW(instance, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (copied == 0) return std::nullopt;
        if (copied < buffer.size() - 1) {
            buffer.resize(copied);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

// Both frontends use the x64 bin directory for the backend and shared assets.
inline std::filesystem::path shared_binary_directory(const std::filesystem::path& module_directory) {
    if (module_directory.filename() == L"x86") return module_directory.parent_path();
    if (module_directory.filename() == L"bin" &&
        module_directory.parent_path().filename() == L"frontend-x86") {
        return module_directory.parent_path().parent_path() / "bin";
    }
    return module_directory;
}

}  // namespace tsf::paths
