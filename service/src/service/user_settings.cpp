#include "user_settings.hpp"
#include "user_settings_codec.hpp"

#include <shlobj.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>

namespace llavon::service {
namespace {

using llavon::ime::core::InferenceDeviceSelection;
constexpr wchar_t settings_directory_name[] = L"Llavon IME";
constexpr wchar_t settings_filename[] = L"settings.json";
std::mutex settings_mutex;

std::filesystem::path settings_path() {
    PWSTR local_app_data = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data);
    if (FAILED(result)) {
        throw std::runtime_error("unable to resolve LocalAppData");
    }
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned_path(
        local_app_data, CoTaskMemFree);
    return std::filesystem::path(owned_path.get()) /
           settings_directory_name / settings_filename;
}

UserSettings load_settings_unlocked() {
    const auto path = settings_path();
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }

    const std::string body{
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    auto settings = user_settings_codec::decode(body);
    if (!settings) {
        std::clog << "[SRV] ignoring invalid settings\n";
        return {};
    }
    return std::move(*settings);
}

bool save_settings_unlocked(const UserSettings& settings) {
    const auto path = settings_path();
    std::filesystem::create_directories(path.parent_path());

    std::filesystem::path temporary = path;
    temporary += L".tmp";
    try {
        bool write_succeeded = false;
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                DeleteFileW(temporary.c_str());
                return false;
            }
            const std::string body = user_settings_codec::encode(settings);
            output.write(body.data(), static_cast<std::streamsize>(body.size()));
            output.flush();
            write_succeeded = static_cast<bool>(output);
        }
        if (!write_succeeded) {
            DeleteFileW(temporary.c_str());
            return false;
        }

        if (!MoveFileExW(temporary.c_str(), path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temporary.c_str());
            return false;
        }
        return true;
    } catch (...) {
        DeleteFileW(temporary.c_str());
        throw;
    }
}

}  // namespace

UserSettings load_settings() noexcept {
    try {
        std::lock_guard lock(settings_mutex);
        return load_settings_unlocked();
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to load settings: " << error.what() << '\n';
        return {};
    } catch (...) {
        std::clog << "[SRV] unable to load settings\n";
        return {};
    }
}

bool save_inference_settings(const InferenceDeviceSelection& selection) noexcept {
    try {
        std::lock_guard lock(settings_mutex);
        auto settings = load_settings_unlocked();
        settings.inference = selection;
        return save_settings_unlocked(settings);
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to save inference settings: " << error.what() << '\n';
        return false;
    } catch (...) {
        std::clog << "[SRV] unable to save inference settings\n";
        return false;
    }
}

bool save_model_path(std::string model_path) noexcept {
    try {
        std::lock_guard lock(settings_mutex);
        auto settings = load_settings_unlocked();
        settings.model_path = std::move(model_path);
        return save_settings_unlocked(settings);
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to save model path: " << error.what() << '\n';
        return false;
    } catch (...) {
        std::clog << "[SRV] unable to save model path\n";
        return false;
    }
}

bool save_custom_names(const std::vector<CustomNameSetting>& custom_names) noexcept {
    try {
        std::lock_guard lock(settings_mutex);
        auto settings = load_settings_unlocked();
        settings.custom_names = custom_names;
        return save_settings_unlocked(settings);
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to save custom name settings: " << error.what() << '\n';
        return false;
    } catch (...) {
        std::clog << "[SRV] unable to save custom name settings\n";
        return false;
    }
}

bool save_shift_space_width_toggle_setting(bool enabled) noexcept {
    try {
        std::lock_guard lock(settings_mutex);
        auto settings = load_settings_unlocked();
        settings.shift_space_width_toggle_enabled = enabled;
        return save_settings_unlocked(settings);
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to save Shift+Space width setting: "
                  << error.what() << '\n';
        return false;
    } catch (...) {
        std::clog << "[SRV] unable to save Shift+Space width setting\n";
        return false;
    }
}

}  // namespace llavon::service
