#include "service/model_revision_cleanup.hpp"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;
using llavon::service::remove_stale_model_revisions;

namespace {

const std::wstring current(40, L'a');
const std::wstring stale(40, L'b');
const std::wstring nested(40, L'c');
const std::wstring junction_inside(40, L'd');
const std::wstring symlink_inside(40, L'e');
const std::wstring junction_revision(40, L'f');
const std::wstring locked_revision = std::wstring(39, L'1') + L'2';

void write_file(const fs::path& path, std::string_view content = "data") {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
}

std::string read_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

bool present(const fs::path& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
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
    return exit_code == 0;
}

int run(const fs::path& sandbox, bool& symlinks_tested) {
    const auto root = sandbox / L"models";
    const auto victim = sandbox / L"victim";
    const auto victim_revision = sandbox / L"victim-revision";
    write_file(victim / L"important.txt", "important");
    write_file(victim_revision / L"model.gguf", "victim-model");

    write_file(root / current / L"model.gguf", "current");
    write_file(root / current / L"model.gguf.partial.123");
    write_file(root / current / L"notes.txt");
    write_file(root / stale / L"model.gguf");
    write_file(root / stale / L"model.gguf.partial.456");
    write_file(root / nested / L"model.gguf");
    write_file(root / nested / L"sub" / L"keep.txt");
    write_file(root / junction_inside / L"model.gguf");
    if (!make_junction(root / junction_inside / L"evil", victim)) return 100;
    write_file(root / symlink_inside / L"model.gguf");
    symlinks_tested = CreateSymbolicLinkW(
        (root / symlink_inside / L"evil.txt").c_str(), (victim / L"important.txt").c_str(),
        SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
    if (!make_junction(root / junction_revision, victim_revision)) return 101;
    write_file(root / locked_revision / L"locked.gguf");
    write_file(root / locked_revision / L"free.gguf");
    write_file(root / L"notes" / L"keep.txt");
    write_file(root / std::wstring(39, L'b') / L"keep.txt");
    write_file(root / (std::wstring(40, L'g')) / L"keep.txt");
    write_file(root / std::wstring(40, L'9'), "file-named-like-revision");
    write_file(root / L"current.revision", "aaaa");

    remove_stale_model_revisions(root, L"");
    remove_stale_model_revisions(root, L"not-a-revision");
    remove_stale_model_revisions(L"relative\\models", current);
    if (!present(root / stale / L"model.gguf")) return 1;

    const HANDLE locked = CreateFileW((root / locked_revision / L"locked.gguf").c_str(),
                                      GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) return 102;
    std::wstring upper_current = current;
    for (auto& c : upper_current) c = static_cast<wchar_t>(towupper(c));
    remove_stale_model_revisions(root, upper_current);
    CloseHandle(locked);

    if (read_file(root / current / L"model.gguf") != "current") return 2;
    if (present(root / current / L"model.gguf.partial.123")) return 3;
    if (!present(root / current / L"notes.txt")) return 4;
    if (present(root / stale)) return 5;
    if (present(root / nested / L"model.gguf")) return 6;
    if (!present(root / nested / L"sub" / L"keep.txt")) return 7;
    if (present(root / junction_inside / L"model.gguf")) return 8;
    if (read_file(victim / L"important.txt") != "important") return 9;
    if (present(root / symlink_inside / L"model.gguf")) return 10;
    if (read_file(victim_revision / L"model.gguf") != "victim-model") return 11;
    if (!present(root / junction_revision)) return 12;
    if (!present(root / locked_revision / L"locked.gguf")) return 13;
    if (present(root / locked_revision / L"free.gguf")) return 14;
    if (!present(root / L"notes" / L"keep.txt")) return 15;
    if (!present(root / std::wstring(39, L'b') / L"keep.txt")) return 16;
    if (!present(root / std::wstring(40, L'g') / L"keep.txt")) return 17;
    if (read_file(root / std::wstring(40, L'9')) != "file-named-like-revision") return 18;
    if (!present(root / L"current.revision")) return 19;

    remove_stale_model_revisions(root, current);
    if (present(root / locked_revision)) return 20;
    remove_stale_model_revisions(sandbox / L"missing", current);
    return 0;
}

}  // namespace

int wmain() {
    const auto sandbox = fs::temp_directory_path() /
                         (L"llavon-model-cleanup-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ignored;
    fs::create_directories(sandbox, ignored);
    bool symlinks_tested = false;
    const int result = run(sandbox, symlinks_tested);
    RemoveDirectoryW((sandbox / L"models" / junction_revision).c_str());
    RemoveDirectoryW((sandbox / L"models" / junction_inside / L"evil").c_str());
    fs::remove_all(sandbox, ignored);
    if (!symlinks_tested)
        std::wcout << L"symbolic link case skipped: creation requires Developer Mode\n";
    if (result != 0) std::wcerr << L"failed with code " << result << L'\n';
    return result;
}
