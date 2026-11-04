#include <windows.h>
#include <msctf.h>

#include "system/globals.h"

#include <cstdio>
#include <initializer_list>

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 2) {
        std::fputs("Expected the absolute path to the TSF DLL.\n", stderr);
        return 1;
    }

    // Resolve dependencies without the build/installation directory, PATH, or
    // the current directory. AppContainer TSF hosts cannot rely on those paths.
    // Resolve the COM factory directly; never register or activate the service.
    const HMODULE module = LoadLibraryExW(argv[1], nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module) {
        const DWORD error = GetLastError();
        std::fprintf(stderr, "TSF DLL failed to load with system dependencies only: error %lu\n", error);
        return 1;
    }
    bool valid = true;
    for (const auto* name : {"DllGetClassObject", "DllCanUnloadNow", "DllRegisterServer", "DllUnregisterServer"}) {
        if (!GetProcAddress(module, name)) {
            std::fprintf(stderr, "Missing undecorated COM export: %s\n", name);
            valid = false;
        }
    }
    using GetClassObject = HRESULT (STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto get_class_object = reinterpret_cast<GetClassObject>(GetProcAddress(module, "DllGetClassObject"));
    if (get_class_object) {
        IClassFactory* factory = nullptr;
        const HRESULT factory_result = get_class_object(tsf::Globals::text_service_clsid, IID_IClassFactory,
                                                        reinterpret_cast<void**>(&factory));
        if (FAILED(factory_result) || !factory) {
            std::fprintf(stderr, "COM factory failed: 0x%08lX\n", static_cast<unsigned long>(factory_result));
            valid = false;
        } else {
            ITfTextInputProcessor* processor = nullptr;
            const HRESULT result = factory->CreateInstance(nullptr, IID_ITfTextInputProcessor,
                                                           reinterpret_cast<void**>(&processor));
            if (FAILED(result) || !processor) {
                std::fprintf(stderr, "COM instance creation failed: 0x%08lX\n", static_cast<unsigned long>(result));
                valid = false;
            }
            if (processor) processor->Release();
            factory->Release();
        }
    }
    FreeLibrary(module);
    return valid ? 0 : 1;
}
