#pragma once

#include <windows.h>

#include <filesystem>
#include <vector>

namespace llavon::uninstall {

inline constexpr wchar_t product_name[] = L"Llavon IME";
static_assert(product_name[0] != L'\0');

struct PurgeTargets {
    std::filesystem::path program_data;
    std::filesystem::path program_files;
    std::vector<std::filesystem::path> user_profiles;
    HKEY software = nullptr;
};

std::vector<std::filesystem::path> user_profile_directories(HKEY profile_list);
void remove_tree(const std::filesystem::path& path) noexcept;
void purge(const PurgeTargets& targets) noexcept;

}  // namespace llavon::uninstall
