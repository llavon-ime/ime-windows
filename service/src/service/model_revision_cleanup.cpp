#include "model_revision_cleanup.hpp"

#include <windows.h>

#include <algorithm>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace llavon::service {

namespace {

constexpr std::size_t revision_length = 40;

bool is_revision_name(std::wstring_view name) noexcept {
    return name.size() == revision_length && std::ranges::all_of(name, [](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') ||
               (c >= L'A' && c <= L'F');
    });
}

bool equal_ignore_case(std::wstring_view left, std::wstring_view right) noexcept {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
                                static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

void remove_revision_files(const std::filesystem::path& directory, bool partial_only) {
    std::error_code error;
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator entry(directory, error), end;
         !error && entry != end; entry.increment(error)) {
        std::error_code status_error;
        if (entry->symlink_status(status_error).type() != std::filesystem::file_type::regular)
            continue;
        if (partial_only &&
            entry->path().filename().native().find(L".partial.") == std::wstring::npos)
            continue;
        files.push_back(entry->path());
    }
    for (const auto& file : files) std::filesystem::remove(file, error);
    if (!partial_only) std::filesystem::remove(directory, error);
}

}  // namespace

void remove_stale_model_revisions(const std::filesystem::path& root,
                                  std::wstring_view current_revision) noexcept {
    try {
        if (!root.is_absolute() || !is_revision_name(current_revision)) return;
        std::error_code error;
        std::vector<std::pair<std::filesystem::path, bool>> revisions;
        for (std::filesystem::directory_iterator entry(root, error), end;
             !error && entry != end; entry.increment(error)) {
            std::error_code status_error;
            const std::wstring name = entry->path().filename().native();
            if (!is_revision_name(name) ||
                entry->symlink_status(status_error).type() !=
                    std::filesystem::file_type::directory) {
                continue;
            }
            revisions.emplace_back(entry->path(), equal_ignore_case(name, current_revision));
        }
        for (const auto& [directory, is_current] : revisions)
            remove_revision_files(directory, is_current);
    } catch (...) {
    }
}

}  // namespace llavon::service
