#include "update_installer.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <sodium.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace llavon::settings {
namespace {

std::filesystem::path updates_directory() {
    PWSTR known_folder = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                    nullptr, &known_folder))) {
        throw std::runtime_error("unable to locate LocalAppData");
    }
    const std::filesystem::path root(known_folder);
    CoTaskMemFree(known_folder);
    return root / L"Llavon IME" / L"updates";
}

std::wstring sha256_file(const std::filesystem::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("unable to open downloaded setup");
    struct CloseFile {
        HANDLE handle;
        ~CloseFile() { CloseHandle(handle); }
    } close{file};
    crypto_hash_sha256_state state{};
    crypto_hash_sha256_init(&state);
    std::array<unsigned char, 256 * 1024> buffer{};
    for (;;) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &count, nullptr)) {
            throw std::runtime_error("unable to read downloaded setup");
        }
        if (count == 0) break;
        crypto_hash_sha256_update(&state, buffer.data(), count);
    }
    std::array<unsigned char, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256_final(&state, digest.data());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    std::wstring result;
    for (const char value : std::string_view(hex.data())) {
        result.push_back(static_cast<wchar_t>(value));
    }
    return result;
}

bool matches_asset(const std::filesystem::path& path, const SetupAsset& asset) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
           std::filesystem::file_size(path, error) == asset.size && !error &&
           sha256_file(path) == asset.sha256;
}

void launch_setup(const std::filesystem::path& setup, const SetupAsset& asset) {
    const std::wstring filename = setup.wstring();
    const std::wstring directory = setup.parent_path().wstring();
    // Keep writes and renames blocked through elevation. Recheck after taking
    // the lock so a local process cannot substitute the file between hashing
    // and ShellExecuteExW.
    const HANDLE file = CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "unable to lock setup");
    }
    struct CloseFile {
        HANDLE handle;
        ~CloseFile() { CloseHandle(handle); }
    } locked{file};
    if (!matches_asset(setup, asset))
        throw std::runtime_error("setup changed before launch");
    SHELLEXECUTEINFOW command{};
    command.cbSize = sizeof(command);
    command.fMask = SEE_MASK_NOCLOSEPROCESS;
    command.lpVerb = L"runas";
    command.lpFile = filename.c_str();
    command.lpParameters = L"-quiet -norestart";
    command.lpDirectory = directory.c_str();
    command.nShow = SW_HIDE;
    if (!ShellExecuteExW(&command)) {
        throw std::system_error(static_cast<int>(GetLastError()),
                                std::system_category(), "unable to start setup");
    }
    if (command.hProcess) CloseHandle(command.hProcess);
}

void install(SetupAsset asset, llavon::service::WinrtHttpTransfer& transfer,
             const UpdateInstaller::Callback& callback) {
    if (sodium_init() < 0) throw std::runtime_error("unable to initialize SHA-256");
    const auto directory = updates_directory();
    std::filesystem::create_directories(directory);
    const auto setup = directory / L"setup.exe";
    if (!matches_asset(setup, asset)) {
        const auto partial = directory /
            (L"setup.partial." + std::to_wstring(GetCurrentProcessId()));
        struct RemovePartial {
            std::filesystem::path path;
            ~RemovePartial() {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        } cleanup{partial};
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("unable to create setup download");
        std::uint64_t last_reported = 0;
        transfer.get_stream(asset.url,
            [&](const std::uint8_t* bytes, std::uint32_t count,
                std::uint64_t received, std::uint64_t) {
                if (received > asset.size)
                    throw std::runtime_error("setup exceeds expected size");
                output.write(reinterpret_cast<const char*>(bytes), count);
                if (!output) throw std::runtime_error("unable to write setup download");
                if (received - last_reported >= 1024 * 1024 || received == asset.size) {
                    callback({UpdateInstallStage::downloading, received, asset.size, {}});
                    last_reported = received;
                }
            });
        output.close();
        if (!output || !matches_asset(partial, asset))
            throw std::runtime_error("setup size or SHA-256 mismatch");
        if (!MoveFileExW(partial.c_str(), setup.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(), "unable to save setup");
        }
    }
    callback({UpdateInstallStage::launching, asset.size, asset.size, {}});
    launch_setup(setup, asset);
    callback({UpdateInstallStage::launched, asset.size, asset.size, {}});
}

std::wstring describe_error(const std::exception& error) {
    const std::string description(error.what());
    return std::wstring(description.begin(), description.end());
}

void report(const UpdateInstaller::Callback& callback, UpdateInstallEvent event) noexcept {
    try {
        callback(std::move(event));
    } catch (...) {
        // The settings window may be closing while the installer continues.
    }
}

}  // namespace

UpdateInstaller::~UpdateInstaller() {
    cancel();
    if (worker_.joinable()) worker_.join();
}

bool UpdateInstaller::install_async(SetupAsset asset, Callback callback) {
    if (!callback || installing_.exchange(true, std::memory_order_acq_rel))
        return false;
    if (worker_.joinable()) worker_.join();
    transfer_.reset();
    worker_ = std::thread([this, asset = std::move(asset),
                           callback = std::move(callback)]() mutable {
        try {
            install(std::move(asset), transfer_, callback);
        } catch (const std::exception& error) {
            report(callback, {UpdateInstallStage::failed, 0, 0, describe_error(error)});
        } catch (...) {
            report(callback, {UpdateInstallStage::failed, 0, 0, L"unknown update error"});
        }
        installing_.store(false, std::memory_order_release);
    });
    return true;
}

void UpdateInstaller::cancel() noexcept {
    transfer_.cancel();
}

}  // namespace llavon::settings
