#pragma once

#include <filesystem>
#include <string>

namespace llavon::service {

// Hashes the exact training model inputs using an unambiguous, versioned
// filename/size/content framing. The result is lowercase SHA-256.
std::string compute_base_model_fingerprint(
    const std::filesystem::path& model_directory);

}  // namespace llavon::service
