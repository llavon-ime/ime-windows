#include <unknwn.h>
#include <windows.h>
#include <winrt/base.h>

#include "system/classFactory.hpp"
#include "system/globals.h"
#include "system/register.hpp"

// #include "tsf/classFactory.h"
// #include "tsf/globals.h"
// #include "tsf/register.h"

/**
 * @brief DLL entry point.
 */
BOOL APIENTRY DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID /*lpReserved*/) {
    switch (dwReason) {
        case DLL_PROCESS_ATTACH:
            tsf::Globals::hinstance = hInstance;
            DisableThreadLibraryCalls(hInstance);
            break;

        case DLL_PROCESS_DETACH:
            break;
    }
    return TRUE;
}

/**
 * @brief COM standard export functions
 */
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    if (!ppv) return E_INVALIDARG;
    *ppv = nullptr;
    // RETURN_HR_IF(CLASS_E_CLASSNOTAVAILABLE, !IsEqualCLSID(rclsid, c_clsidTextService));
    if (!IsEqualCLSID(rclsid, tsf::Globals::text_service_clsid)) return CLASS_E_CLASSNOTAVAILABLE;
    try {
        auto factory = winrt::make<tsf::ClassFactory>();
        return factory->QueryInterface(riid, ppv);
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

STDAPI DllCanUnloadNow() {
    return (tsf::Globals::dll_ref_count == 0) ? S_OK : S_FALSE;
}

/**
 * @brief Self-registration / unregistration (called by regsvr32, requires admin privileges)
 */
STDAPI DllRegisterServer() {
    return RegisterServer();
}

STDAPI DllUnregisterServer() {
    return UnregisterServer();
}
