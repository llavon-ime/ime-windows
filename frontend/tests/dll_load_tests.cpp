#include <windows.h>

#include <cstdio>

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        std::fputs("Expected the absolute path to the TSF DLL.\n", stderr);
        return 1;
    }

    // Resolve dependencies without the build/installation directory, PATH, or
    // the current directory. AppContainer TSF hosts cannot rely on those paths.
    // This deliberately tests loading only; it never registers the text service.
    const HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "TSF DLL failed to load with system dependencies only: error %lu\n", error);
        return 1;
    }
    FreeLibrary(module);
    return 0;
}
