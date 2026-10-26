#include "uninstall_purge.hpp"

#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <vector>

namespace fs = std::filesystem;
using llavon::uninstall::PurgeTargets;
using llavon::uninstall::purge;
using llavon::uninstall::remove_tree;
using llavon::uninstall::user_profile_directories;

namespace {

using UniqueKey = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;

fs::path extended(const fs::path& path) {
    return fs::path(L"\\\\?\\" + path.lexically_normal().native());
}

void write_file(const fs::path& path, std::string_view content = "data") {
    fs::create_directories(extended(path.parent_path()));
    std::ofstream output(extended(path), std::ios::binary | std::ios::trunc);
    output << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(extended(path), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool present(const fs::path& path) {
    return GetFileAttributesW(extended(path).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool is_reparse_point(const fs::path& path) {
    const DWORD attributes = GetFileAttributesW(extended(path).c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool make_junction(const fs::path& link, const fs::path& target) {
    fs::create_directories(link.parent_path());
    std::wstring command = L"cmd.exe /d /c mklink /J \"" + link.native() + L"\" \"" +
                           target.native() + L"\" >nul";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0 && is_reparse_point(link);
}

bool make_symlink(const fs::path& link, const fs::path& target, bool directory) {
    fs::create_directories(link.parent_path());
    const DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE |
                        (directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0);
    return CreateSymbolicLinkW(link.c_str(), target.c_str(), flags) != 0;
}

int run_process(std::wstring command) {
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exit_code);
}

UniqueKey load_hive(const fs::path& file) {
    HKEY raw = nullptr;
    if (RegLoadAppKeyW(file.c_str(), &raw, KEY_ALL_ACCESS, 0, 0) != ERROR_SUCCESS)
        raw = nullptr;
    return UniqueKey(raw, RegCloseKey);
}

bool create_key(HKEY root, const wchar_t* path) {
    HKEY raw = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &raw, nullptr) !=
        ERROR_SUCCESS) {
        return false;
    }
    const DWORD value = 1;
    const bool written = RegSetValueExW(raw, L"Value", 0, REG_DWORD,
                                        reinterpret_cast<const BYTE*>(&value),
                                        sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(raw);
    return written;
}

bool set_string(HKEY root, const wchar_t* key, DWORD type, std::wstring_view value) {
    return RegSetKeyValueW(root, key, L"ProfileImagePath", type, value.data(),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) ==
           ERROR_SUCCESS;
}

bool key_exists(HKEY root, const wchar_t* path) {
    HKEY raw = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &raw) != ERROR_SUCCESS) return false;
    RegCloseKey(raw);
    return true;
}

int test_filesystem_layout(const fs::path& sandbox, bool& symlinks_tested) {
    const auto program_data = sandbox / L"ProgramData";
    const auto program_files = sandbox / L"ProgramFiles";
    const auto victim = sandbox / L"victim";
    const auto victim_profile_target = sandbox / L"victim-profile-target";
    const auto user1 = sandbox / L"Users" / L"user1";
    const auto user2 = sandbox / L"Users" / L"user2";
    const auto user3 = sandbox / L"Users" / L"user3";
    const auto user1_product = user1 / L"AppData" / L"Local" / L"Llavon IME";

    write_file(program_data / L"Llavon IME" / L"models" / L"rev" / L"model.gguf");
    write_file(program_data / L"Llavon IME" / L"models" / L"current.revision");
    write_file(program_data / L"Llavon IME" / L"models" / L"rev" / L"model.gguf.partial.42");
    SetFileAttributesW(
        extended(program_data / L"Llavon IME" / L"models" / L"rev" / L"model.gguf").c_str(),
        FILE_ATTRIBUTE_READONLY);
    write_file(program_data / L"Other App" / L"keep.txt");
    write_file(program_data / L"Llavon IME Backup" / L"keep.txt");
    write_file(program_data / L"Llavon IME.txt");

    write_file(program_files / L"Llavon IME" / L"tools" / L"lora" / L"trainer.exe");
    write_file(program_files / L"Llavon IME" / L"bin" / L"keep.dll");
    write_file(program_files / L"Other App" / L"tools" / L"keep.exe");

    write_file(victim / L"important.txt", "important");
    write_file(victim / L"sub" / L"deep.txt", "deep");
    write_file(victim / L"hard.txt", "hard");
    write_file(victim_profile_target / L"keep.txt", "profile-target");

    write_file(user1_product / L"training" / L"data.db");
    write_file(user1_product / L"settings.json");
    fs::path deep = user1_product;
    for (int depth = 0; depth < 30; ++depth) deep /= L"nested-dir";
    write_file(deep / L"deep.bin");
    write_file(user1 / L"AppData" / L"Local" / L"Other" / L"keep.txt");
    write_file(user1 / L"AppData" / L"Roaming" / L"Llavon IME" / L"keep.txt");
    write_file(user1 / L"Documents" / L"Llavon IME" / L"keep.txt");
    if (!make_junction(user1_product / L"junction-to-victim", victim)) return 100;
    if (!CreateHardLinkW(extended(user1_product / L"hardlink.txt").c_str(),
                         extended(victim / L"hard.txt").c_str(), nullptr)) {
        return 101;
    }
    symlinks_tested = make_symlink(user1_product / L"symlink-dir", victim, true) &&
                      make_symlink(user1_product / L"symlink-file",
                                   victim / L"important.txt", false);

    fs::create_directories(user2);
    if (!make_junction(user3 / L"AppData" / L"Local" / L"Llavon IME", victim_profile_target))
        return 102;

    const auto cwd = sandbox / L"cwd";
    write_file(cwd / L"Llavon IME" / L"keep.txt");
    write_file(cwd / L"relative-profile" / L"AppData" / L"Local" / L"Llavon IME" / L"keep.txt");
    write_file(cwd / L"relative-files" / L"Llavon IME" / L"tools" / L"keep.txt");
    const auto previous_cwd = fs::current_path();
    fs::current_path(cwd);

    PurgeTargets targets{
        .program_data = program_data,
        .program_files = program_files,
        .user_profiles = {user1, user2, user3, L"", L"relative-profile",
                          sandbox / L"Users" / L"missing"},
    };
    purge(targets);
    purge(PurgeTargets{.program_data = L"", .program_files = L"relative-files"});
    fs::current_path(previous_cwd);

    if (present(program_data / L"Llavon IME")) return 1;
    if (!present(program_data / L"Other App" / L"keep.txt")) return 2;
    if (!present(program_data / L"Llavon IME Backup" / L"keep.txt")) return 3;
    if (!present(program_data / L"Llavon IME.txt")) return 4;

    if (present(program_files / L"Llavon IME" / L"tools")) return 5;
    if (!present(program_files / L"Llavon IME" / L"bin" / L"keep.dll")) return 6;
    if (!present(program_files / L"Other App" / L"tools" / L"keep.exe")) return 7;

    if (present(user1_product)) return 8;
    if (!present(user1 / L"AppData" / L"Local" / L"Other" / L"keep.txt")) return 9;
    if (!present(user1 / L"AppData" / L"Roaming" / L"Llavon IME" / L"keep.txt")) return 10;
    if (!present(user1 / L"Documents" / L"Llavon IME" / L"keep.txt")) return 11;

    if (read_file(victim / L"important.txt") != "important") return 12;
    if (read_file(victim / L"sub" / L"deep.txt") != "deep") return 13;
    if (read_file(victim / L"hard.txt") != "hard") return 14;

    if (present(user3 / L"AppData" / L"Local" / L"Llavon IME")) return 15;
    if (read_file(victim_profile_target / L"keep.txt") != "profile-target") return 16;
    if (!present(user2)) return 17;

    if (!present(cwd / L"Llavon IME" / L"keep.txt")) return 18;
    if (!present(cwd / L"relative-profile" / L"AppData" / L"Local" / L"Llavon IME" /
                 L"keep.txt")) {
        return 19;
    }
    if (!present(cwd / L"relative-files" / L"Llavon IME" / L"tools" / L"keep.txt")) return 20;
    return 0;
}

int test_program_files_root(const fs::path& sandbox) {
    const auto program_files = sandbox / L"ProgramFilesOnlyTools";
    write_file(program_files / L"Llavon IME" / L"tools" / L"lora" / L"trainer.exe");
    write_file(program_files / L"Neighbour" / L"keep.txt");
    purge(PurgeTargets{.program_files = program_files});
    if (present(program_files / L"Llavon IME")) return 30;
    if (!present(program_files / L"Neighbour" / L"keep.txt")) return 31;

    const auto linked_files = sandbox / L"ProgramFilesJunction";
    const auto relocated = sandbox / L"relocated-install";
    write_file(relocated / L"tools" / L"keep.exe", "relocated");
    write_file(relocated / L"bin" / L"keep.dll");
    if (!make_junction(linked_files / L"Llavon IME", relocated)) return 32;
    purge(PurgeTargets{.program_files = linked_files});
    if (read_file(relocated / L"tools" / L"keep.exe") != "relocated") return 33;
    if (!present(relocated / L"bin" / L"keep.dll")) return 34;
    if (!is_reparse_point(linked_files / L"Llavon IME")) return 35;
    return 0;
}

int test_locked_file(const fs::path& sandbox) {
    const auto program_data = sandbox / L"ProgramDataLocked";
    write_file(program_data / L"Llavon IME" / L"locked.bin");
    write_file(program_data / L"Llavon IME" / L"free.bin");
    const HANDLE locked = CreateFileW(
        extended(program_data / L"Llavon IME" / L"locked.bin").c_str(), GENERIC_READ, 0,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 40;
    purge(PurgeTargets{.program_data = program_data});
    CloseHandle(locked);
    if (present(program_data / L"Llavon IME" / L"free.bin")) return 41;
    if (!present(program_data / L"Llavon IME" / L"locked.bin")) return 42;
    purge(PurgeTargets{.program_data = program_data});
    if (present(program_data / L"Llavon IME")) return 43;
    return 0;
}

int test_remove_tree_guards(const fs::path& sandbox) {
    const auto cwd = sandbox / L"guard-cwd";
    write_file(cwd / L"keep.txt");
    const auto previous_cwd = fs::current_path();
    fs::current_path(cwd);
    remove_tree(L"");
    remove_tree(L".");
    remove_tree(L"keep.txt");
    remove_tree(L"\\");
    fs::current_path(previous_cwd);
    if (!present(cwd / L"keep.txt")) return 50;
    remove_tree(sandbox / L"does-not-exist");
    purge(PurgeTargets{});
    return 0;
}

int test_registry(const fs::path& sandbox) {
    const auto hive_file = sandbox / L"software.hive";
    {
        const auto hive = load_hive(hive_file);
        if (!hive) return 60;
        if (!create_key(hive.get(), L"Llavon IME") ||
            !create_key(hive.get(), L"Llavon IME\\Nested\\Deeper") ||
            !create_key(hive.get(), L"Llavon IME Backup") ||
            !create_key(hive.get(), L"Other Vendor\\Llavon IME") ||
            !create_key(hive.get(), L"Microsoft")) {
            return 61;
        }
        purge(PurgeTargets{.software = hive.get()});
        if (key_exists(hive.get(), L"Llavon IME")) return 62;
        if (!key_exists(hive.get(), L"Llavon IME Backup")) return 63;
        if (!key_exists(hive.get(), L"Other Vendor\\Llavon IME")) return 64;
        if (!key_exists(hive.get(), L"Microsoft")) return 65;
        purge(PurgeTargets{.software = hive.get()});
        if (!key_exists(hive.get(), L"Microsoft")) return 66;
    }
    return 0;
}

int test_profile_list(const fs::path& sandbox) {
    const auto hive_file = sandbox / L"profiles.hive";
    const auto hive = load_hive(hive_file);
    if (!hive) return 70;
    if (!set_string(hive.get(), L"S-1-5-21-1", REG_EXPAND_SZ, L"%SystemDrive%\\Users\\alice") ||
        !set_string(hive.get(), L"S-1-5-21-2", REG_SZ, L"D:\\Profiles\\bob") ||
        !set_string(hive.get(), L"S-1-5-21-3", REG_SZ, L"relative\\carol") ||
        !set_string(hive.get(), L"S-1-5-21-4", REG_SZ, L"") ||
        !create_key(hive.get(), L"S-1-5-21-5") ||
        !set_string(hive.get(), L"S-1-5-21-6", REG_DWORD, L"x")) {
        return 71;
    }
    wchar_t expanded[MAX_PATH]{};
    if (ExpandEnvironmentStringsW(L"%SystemDrive%\\Users\\alice", expanded,
                                  ARRAYSIZE(expanded)) == 0) {
        return 72;
    }
    auto profiles = user_profile_directories(hive.get());
    std::ranges::sort(profiles);
    std::vector<fs::path> expected{fs::path(expanded), fs::path(L"D:\\Profiles\\bob")};
    std::ranges::sort(expected);
    if (profiles != expected) return 73;
    return 0;
}

int test_executable(const fs::path& executable) {
    const std::wstring quoted = L"\"" + executable.native() + L"\"";
    if (run_process(quoted) != ERROR_INVALID_PARAMETER) return 80;
    if (run_process(quoted + L" bogus") != ERROR_INVALID_PARAMETER) return 81;
    if (run_process(quoted + L" purge extra") != ERROR_INVALID_PARAMETER) return 82;
    if (run_process(quoted + L" PURGE") != ERROR_INVALID_PARAMETER) return 83;
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) {
        std::wcerr << L"usage: uninstall-purge-tests <llavon-ime-uninstaller.exe>\n";
        return 2;
    }
    const auto sandbox = fs::temp_directory_path() /
                         (L"llavon-uninstall-test-" + std::to_wstring(GetCurrentProcessId()));
    remove_tree(sandbox);
    fs::create_directories(sandbox);

    bool symlinks_tested = false;
    int result = test_filesystem_layout(sandbox, symlinks_tested);
    if (result == 0) result = test_program_files_root(sandbox);
    if (result == 0) result = test_locked_file(sandbox);
    if (result == 0) result = test_remove_tree_guards(sandbox);
    if (result == 0) result = test_registry(sandbox);
    if (result == 0) result = test_profile_list(sandbox);
    if (result == 0) result = test_executable(argv[1]);

    remove_tree(sandbox);
    if (!symlinks_tested)
        std::wcout << L"symbolic link cases skipped: creation requires Developer Mode\n";
    if (result != 0) std::wcerr << L"failed with code " << result << L'\n';
    return result;
}
