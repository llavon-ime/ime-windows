#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windows.h>

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <type_traits>

#include "system/globals.h"
#include "uninstall_purge.hpp"

namespace {

constexpr wchar_t profile_list_key[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
constexpr DWORD install_layout_uninstall = 0x00000001;

using UniqueKey = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;

int remove_user_input_profile() noexcept {
    const HMODULE input = LoadLibraryExW(L"input.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!input) return static_cast<int>(GetLastError());
    using InstallLayoutOrTip = BOOL(WINAPI*)(LPCWSTR, DWORD);
    const auto install = reinterpret_cast<InstallLayoutOrTip>(
        GetProcAddress(input, "InstallLayoutOrTip"));
    wchar_t clsid[39]{};
    wchar_t profile[39]{};
    int result = ERROR_PROC_NOT_FOUND;
    if (install &&
        StringFromGUID2(tsf::Globals::text_service_clsid, clsid, ARRAYSIZE(clsid)) != 0 &&
        StringFromGUID2(tsf::Globals::text_service_profile_guid, profile,
                        ARRAYSIZE(profile)) != 0) {
        try {
            const auto entry = std::format(L"{:04X}:{}{}",
                                           tsf::Globals::tsf_language_id, clsid, profile);
            result = install(entry.c_str(), install_layout_uninstall)
                         ? ERROR_SUCCESS
                         : ERROR_FUNCTION_FAILED;
        } catch (...) {
            result = ERROR_FUNCTION_FAILED;
        }
    }
    FreeLibrary(input);
    return result;
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw);
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    if (FAILED(result) || !owned) return {};
    return std::filesystem::path(owned.get());
}

UniqueKey open_machine_key(const wchar_t* path, REGSAM access) noexcept {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, access | KEY_WOW64_64KEY, &raw) !=
        ERROR_SUCCESS) {
        raw = nullptr;
    }
    return UniqueKey(raw, RegCloseKey);
}

int purge_installation_data() noexcept {
    try {
        llavon::uninstall::PurgeTargets targets{
            .program_data = known_folder(FOLDERID_ProgramData),
            .program_files = known_folder(FOLDERID_ProgramFiles),
        };
        if (const auto profiles = open_machine_key(profile_list_key, KEY_READ))
            targets.user_profiles = llavon::uninstall::user_profile_directories(profiles.get());
        const auto software = open_machine_key(L"Software", KEY_READ | DELETE);
        targets.software = software.get();
        llavon::uninstall::purge(targets);
        return ERROR_SUCCESS;
    } catch (...) {
        return ERROR_FUNCTION_FAILED;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argument_count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argument_count);
    if (!arguments) return ERROR_INVALID_PARAMETER;
    const std::wstring operation = argument_count == 2 ? arguments[1] : L"";
    LocalFree(arguments);

    if (operation == L"remove-user-input-profile") return remove_user_input_profile();
    if (operation == L"purge") return purge_installation_data();
    return ERROR_INVALID_PARAMETER;
}
