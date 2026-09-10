#pragma once

#include <Unknwn.h>
#include <windows.h>
#include <winrt/base.h>

#include "globals.h"
#include "sysutil.hpp"
#include "tsf/textService.h"

namespace tsf {

/**
 * @brief Class factory for creating TSF service instances
 */
class ClassFactory : public winrt::implements<ClassFactory, IClassFactory>, public module_lock_updater {
public:
    // IClassFactory
    STDMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppv) override {
        if (!ppv) return E_INVALIDARG;
        *ppv = nullptr;
        if (pUnkOuter) return CLASS_E_NOAGGREGATION;

        auto pService = winrt::make<TextService>();
        if (!pService) return E_OUTOFMEMORY;

        return pService->QueryInterface(riid, ppv);
    }
    STDMETHODIMP LockServer(BOOL fLock) override {
        if (fLock)
            ++tsf::Globals::dll_ref_count;
        else
            --tsf::Globals::dll_ref_count;
        return S_OK;
    }
};

}  // namespace tsf
