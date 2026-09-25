#include "service/winrt_http.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr wchar_t model_name[] = L"llavon-ime-llama-250m-Q4_K_M.gguf";

bool is_hex(std::wstring_view value, std::size_t length) {
    return value.size() == length && std::ranges::all_of(value, [](wchar_t c) {
        return (c >= L'0' && c <= L'9') ||
               (c >= L'a' && c <= L'f') ||
               (c >= L'A' && c <= L'F');
    });
}

std::filesystem::path model_root() {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE,
                                    nullptr, &raw)))
        throw std::runtime_error("unable to locate ProgramData");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    return std::filesystem::path(owned.get()) / L"Llavon IME" / L"models";
}

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("unable to open model for verification");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("unable to initialize SHA-256");
    struct AlgorithmCloser {
        BCRYPT_ALG_HANDLE handle;
        ~AlgorithmCloser() { BCryptCloseAlgorithmProvider(handle, 0); }
    } close_algorithm{algorithm};
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("unable to create SHA-256 hash");
    struct HashCloser {
        BCRYPT_HASH_HANDLE handle;
        ~HashCloser() { BCryptDestroyHash(handle); }
    } close_hash{hash};
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        if (count > 0 && BCryptHashData(hash,
                reinterpret_cast<PUCHAR>(buffer.data()),
                static_cast<ULONG>(count), 0) < 0)
            throw std::runtime_error("unable to hash model");
    }
    if (!input.eof()) throw std::runtime_error("unable to read model completely");
    std::array<std::uint8_t, 32> digest{};
    if (BCryptFinishHash(hash, digest.data(),
                         static_cast<ULONG>(digest.size()), 0) < 0)
        throw std::runtime_error("unable to finish SHA-256");
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}

void publish_revision(const std::filesystem::path& root,
                      std::wstring_view revision) {
    const auto partial = root / L"current.revision.partial";
    const auto current = root / L"current.revision";
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        for (const wchar_t c : revision) output.put(static_cast<char>(c));
        output.put('\n');
        output.flush();
        if (!output) throw std::runtime_error("unable to write model revision");
    }
    if (!MoveFileExW(partial.c_str(), current.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ignored;
        std::filesystem::remove(partial, ignored);
        throw std::runtime_error("unable to publish model revision");
    }
}

void install_model(std::wstring_view revision, std::wstring_view expected_sha,
                   std::uint64_t expected_size) {
    if (!is_hex(revision, 40) || !is_hex(expected_sha, 64) || expected_size == 0)
        throw std::invalid_argument("invalid model metadata");
    const auto root = model_root();
    const auto destination = root / std::wstring(revision) / model_name;
    std::string expected;
    expected.reserve(expected_sha.size());
    for (const wchar_t c : expected_sha)
        expected.push_back(static_cast<char>(c));
    std::ranges::transform(expected, expected.begin(), [](char c) {
        return c >= 'A' && c <= 'F' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    std::error_code error;
    if (std::filesystem::is_regular_file(destination, error) &&
        std::filesystem::file_size(destination, error) == expected_size && !error &&
        sha256_file(destination) == expected) {
        publish_revision(root, revision);
        std::wcout << L"Model already verified; download skipped.\n";
        return;
    }
    std::filesystem::create_directories(destination.parent_path());
    const auto partial = destination.wstring() + L".partial." +
                         std::to_wstring(GetCurrentProcessId());
    struct PartialCleanup {
        std::filesystem::path path;
        ~PartialCleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{partial};
    std::ofstream output(partial, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("unable to create model download");
    const std::wstring url =
        L"https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/" +
        std::wstring(revision) + L"/" + model_name + L"?download=true";
    llavon::service::WinrtHttpTransfer transfer;
    std::uint64_t received = 0;
    transfer.get_stream(url, [&](const std::uint8_t* bytes, std::uint32_t count,
                                 std::uint64_t current, std::uint64_t) {
        if (current > expected_size) throw std::runtime_error("model exceeds expected size");
        output.write(reinterpret_cast<const char*>(bytes), count);
        if (!output) throw std::runtime_error("unable to write model download");
        received = current;
    });
    output.close();
    if (received != expected_size || sha256_file(partial) != expected)
        throw std::runtime_error("model size or SHA-256 mismatch");
    if (!MoveFileExW(partial.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("unable to publish verified model");
    publish_revision(root, revision);
    std::wcout << L"Model download complete.\n";
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        if (argc != 5 || std::wstring_view(argv[1]) != L"--install-model")
            throw std::invalid_argument(
                "usage: llavon-ime-model-installer --install-model REVISION SHA256 SIZE");
        const std::wstring_view size_text(argv[4]);
        std::size_t consumed = 0;
        const auto size = std::stoull(std::wstring(size_text), &consumed);
        if (consumed != size_text.size())
            throw std::invalid_argument("invalid model size");
        install_model(argv[2], argv[3], size);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "model installation failed: " << error.what() << '\n';
        return 1;
    }
}
