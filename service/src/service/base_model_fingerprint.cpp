#include "base_model_fingerprint.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace llavon::service {

std::string compute_base_model_fingerprint(
    const std::filesystem::path& directory) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("unable to initialize base model SHA-256");
    struct AlgorithmCloser { BCRYPT_ALG_HANDLE value; ~AlgorithmCloser() {
        BCryptCloseAlgorithmProvider(value, 0); } } algorithm_closer{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("unable to create base model SHA-256");
    struct HashCloser { BCRYPT_HASH_HANDLE value; ~HashCloser() {
        BCryptDestroyHash(value); } } hash_closer{hash};
    const auto append = [&](const void* data, std::size_t size) {
        if (size > std::numeric_limits<ULONG>::max() || BCryptHashData(hash,
                static_cast<PUCHAR>(const_cast<void*>(data)),
                static_cast<ULONG>(size), 0) < 0)
            throw std::runtime_error("unable to hash base model");
    };
    constexpr std::string_view format = "llavon-base-model-fingerprint-v1";
    append(format.data(), format.size());
    constexpr std::array<std::string_view, 3> files{
        "config.json", "ime_vocab.json", "model.safetensors"};
    std::vector<char> buffer(1024 * 1024);
    for (const auto filename : files) {
        const std::uint64_t name_size = filename.size();
        append(&name_size, sizeof(name_size));
        append(filename.data(), filename.size());
        const auto path = directory / filename;
        std::error_code error;
        const std::uint64_t file_size = std::filesystem::file_size(path, error);
        if (error) throw std::runtime_error("installed base model is incomplete");
        append(&file_size, sizeof(file_size));
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("installed base model is unreadable");
        while (input) {
            input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            if (const auto count = input.gcount(); count > 0)
                append(buffer.data(), static_cast<std::size_t>(count));
        }
        if (!input.eof())
            throw std::runtime_error("installed base model could not be read completely");
    }
    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("unable to finish base model SHA-256");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

}  // namespace llavon::service
