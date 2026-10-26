#pragma once

#include <filesystem>
#include <string_view>

namespace llavon::service {

void remove_stale_model_revisions(const std::filesystem::path& root,
                                  std::wstring_view current_revision) noexcept;

}  // namespace llavon::service
