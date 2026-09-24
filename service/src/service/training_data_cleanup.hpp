#pragma once

#include <windows.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>

namespace llavon::service {

// Only application-owned dataset filenames are eligible. Never follow reparse
// points, including junctions in ancestors of the configured cache directory.
inline std::size_t discard_plaintext_training_datasets(const std::filesystem::path& directory) {
    const auto root = std::filesystem::absolute(directory).lexically_normal();
    if (!std::filesystem::exists(root)) return 0;
    for (auto ancestor = root; !ancestor.empty(); ancestor = ancestor.parent_path()) {
        const auto attributes = GetFileAttributesW(ancestor.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("unsafe training cache path");
        }
        if (ancestor == ancestor.parent_path()) break;
    }
    std::size_t removed = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            throw std::runtime_error("unsafe training cache entry");
        }
        const auto filename = entry.path().filename();
        if (entry.is_regular_file() &&
            (filename == L"training.jsonl" || filename == L"training.jsonl.partial")) {
            if (!std::filesystem::remove(entry.path())) throw std::runtime_error("cache removal failed");
            ++removed;
        }
    }
    return removed;
}

} // namespace llavon::service
