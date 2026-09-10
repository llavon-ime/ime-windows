#include "inference_settings.hpp"
#include "inference_settings_codec.hpp"

#include <shlobj.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <utility>

namespace llavon::service {
namespace {

using llavon::ime::core::InferenceDeviceSelection;
constexpr wchar_t settings_directory_name[] = L"Llavon IME";
constexpr wchar_t settings_filename[] = L"settings.json";

std::filesystem::path settings_path() {
    PWSTR local_app_data = nullptr;
    const HRESULT result =
        SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &local_app_data);
    if (FAILED(result)) {
        throw std::runtime_error("unable to resolve LocalAppData");
    }
    try {
        const std::filesystem::path directory =
            std::filesystem::path(local_app_data) / settings_directory_name;
        const std::filesystem::path result_path = directory / settings_filename;
        CoTaskMemFree(local_app_data);
        return result_path;
    } catch (...) {
        CoTaskMemFree(local_app_data);
        throw;
    }
}

}  // namespace

InferenceDeviceSelection load_inference_settings() noexcept {
    try {
        const auto path = settings_path();
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return {};
        }

        const std::string body{
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        auto selection = inference_settings_codec::decode(body);
        if (!selection) {
            std::clog << "[SRV] ignoring invalid inference settings\n";
            return {};
        }
        return std::move(*selection);
    } catch (const std::exception& error) {
        std::clog << "[SRV] unable to load inference settings: " << error.what() << '\n';
        return {};
    } catch (...) {
        std::clog << "[SRV] unable to load inference settings\n";
        return {};
    }
}

bool save_inference_settings(const InferenceDeviceSelection& selection) noexcept {
    std::filesystem::path temporary;
    try {
        const auto path = settings_path();
        std::filesystem::create_directories(path.parent_path());

        temporary = path;
        temporary += L".tmp";
        bool write_succeeded = false;
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) {
                DeleteFileW(temporary.c_str());
                return false;
            }
            const std::string body = inference_settings_codec::encode(selection);
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
    } catch (const std::exception& error) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        std::clog << "[SRV] unable to save inference settings: " << error.what() << '\n';
        return false;
    } catch (...) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        std::clog << "[SRV] unable to save inference settings\n";
        return false;
    }
}

}  // namespace llavon::service
