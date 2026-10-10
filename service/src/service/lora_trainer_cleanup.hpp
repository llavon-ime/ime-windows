#pragma once

#include <windows.h>

#include <array>
#include <filesystem>
#include <string_view>

namespace llavon::service {

inline bool valid_trainer_version(std::string_view value) {
    if (value.size() < 12 || value[4] != '.' || value[7] != '.' ||
        value[10] != '.') return false;
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 4 || index == 7 || index == 10) continue;
        if (value[index] < '0' || value[index] > '9') return false;
    }
    return true;
}

// Call only after publishing current.install. Keep every backend of the selected
// version; remove only known backend directories belonging to other releases.
// A failed cleanup must not turn a successful installation into a failed one.
inline bool prune_obsolete_lora_trainers(const std::filesystem::path& directory,
                                        std::string_view selected_version) noexcept {
    try {
        if (!valid_trainer_version(selected_version)) return false;
        const auto root = std::filesystem::absolute(directory).lexically_normal();
        const auto ordinary_directory = [](const std::filesystem::path& path) {
            const auto attributes = GetFileAttributesW(path.c_str());
            return attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                   (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
        };
        for (auto ancestor = root; ; ancestor = ancestor.parent_path()) {
            if (!ordinary_directory(ancestor)) return false;
            if (ancestor == ancestor.parent_path()) break;
        }
        const auto selected = root / selected_version;
        if (!ordinary_directory(selected)) return false;

        bool complete = true;
        constexpr std::array backends{L"win-x64-cpu", L"win-x64-cuda", L"win-x64-rocm"};
        for (const auto& version : std::filesystem::directory_iterator(root)) {
            if (version.path() == selected ||
                !valid_trainer_version(version.path().filename().string())) continue;
            if (!ordinary_directory(version.path())) {
                complete = false;
                continue;
            }
            for (const auto backend : backends) {
                const auto candidate = version.path() / backend;
                try {
                    if (!std::filesystem::exists(candidate)) continue;
                    if (!ordinary_directory(candidate)) {
                        complete = false;
                        continue;
                    }
                    // Reject junctions and symlinks anywhere in the tree before
                    // removal, so cleanup cannot traverse outside this install.
                    bool safe = true;
                    for (const auto& entry : std::filesystem::recursive_directory_iterator(candidate)) {
                        const auto attributes = GetFileAttributesW(entry.path().c_str());
                        if (attributes == INVALID_FILE_ATTRIBUTES ||
                            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
                            safe = false;
                            break;
                        }
                    }
                    if (safe) std::filesystem::remove_all(candidate);
                    else complete = false;
                } catch (...) {
                    complete = false;
                }
            }
            // Preserve unrelated files and directories within a version folder.
            if (std::filesystem::is_empty(version.path()))
                std::filesystem::remove(version.path());
        }
        return complete;
    } catch (...) {
        return false;
    }
}

} // namespace llavon::service
