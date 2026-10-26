#include "uninstall_purge.hpp"

#include <string>
#include <system_error>

namespace llavon::uninstall {

namespace {

std::wstring extended_path(const std::filesystem::path& path) {
    const std::wstring native = path.lexically_normal().native();
    if (native.starts_with(L"\\\\")) return native;
    return L"\\\\?\\" + native;
}

void remove_entry(const std::wstring& path) noexcept {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
            RemoveDirectoryW(path.c_str());
        } else {
            DeleteFileW(path.c_str());
        }
        return;
    }
    if (attributes & FILE_ATTRIBUTE_READONLY)
        SetFileAttributesW(path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
    if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileW(path.c_str());
        return;
    }
    try {
        std::vector<std::wstring> children;
        std::error_code error;
        for (std::filesystem::directory_iterator entry(path, error), end;
             !error && entry != end; entry.increment(error)) {
            children.push_back(entry->path().native());
        }
        for (const auto& child : children) remove_entry(child);
    } catch (...) {
    }
    RemoveDirectoryW(path.c_str());
}

void remove_product_directory(const std::filesystem::path& base) noexcept {
    if (base.is_absolute()) remove_tree(base / product_name);
}

}  // namespace

std::vector<std::filesystem::path> user_profile_directories(HKEY profile_list) {
    std::vector<std::filesystem::path> profiles;
    for (DWORD index = 0;; ++index) {
        wchar_t sid[256]{};
        DWORD sid_length = ARRAYSIZE(sid);
        const LONG enumerated = RegEnumKeyExW(profile_list, index, sid, &sid_length,
                                              nullptr, nullptr, nullptr, nullptr);
        if (enumerated == ERROR_NO_MORE_ITEMS) break;
        if (enumerated != ERROR_SUCCESS) continue;
        wchar_t profile[MAX_PATH]{};
        DWORD profile_size = sizeof(profile);
        if (RegGetValueW(profile_list, sid, L"ProfileImagePath", RRF_RT_REG_SZ, nullptr,
                         profile, &profile_size) != ERROR_SUCCESS) {
            continue;
        }
        std::filesystem::path path(profile);
        if (path.is_absolute()) profiles.push_back(std::move(path));
    }
    return profiles;
}

void remove_tree(const std::filesystem::path& path) noexcept {
    try {
        if (!path.is_absolute() || path.lexically_normal() == path.root_path()) return;
        remove_entry(extended_path(path));
    } catch (...) {
    }
}

void purge(const PurgeTargets& targets) noexcept {
    try {
        remove_product_directory(targets.program_data);
        for (const auto& profile : targets.user_profiles)
            remove_product_directory(profile / L"AppData" / L"Local");
        if (targets.program_files.is_absolute()) {
            const auto product_root = extended_path(targets.program_files / product_name);
            const DWORD attributes = GetFileAttributesW(product_root.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES &&
                (attributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                remove_tree(targets.program_files / product_name / L"tools");
                RemoveDirectoryW(product_root.c_str());
            }
        }
        if (targets.software) RegDeleteTreeW(targets.software, product_name);
    } catch (...) {
    }
}

}  // namespace llavon::uninstall
