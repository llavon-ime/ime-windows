#pragma once

#include <msctf.h>
#include <objbase.h>
#include <windows.h>

#include <string>

#include "globals.h"

/**
 * @brief Writes a REG_SZ value to an opened registry key.
 */
static HRESULT SetRegString(HKEY hKey, const wchar_t* pszName, const wchar_t* pszValue) {
    DWORD cb = static_cast<DWORD>((wcslen(pszValue) + 1) * sizeof(wchar_t));
    LONG lr = RegSetValueExW(hKey, pszName, 0, REG_SZ, reinterpret_cast<const BYTE*>(pszValue), cb);
    return HRESULT_FROM_WIN32(lr);
}

/**
 * @brief Registers the COM in-process server under HKLM\Software\Classes\CLSID.
 */
static HRESULT RegisterCOMServer(const wchar_t* pszCLSID, const wchar_t* pszDllPath) {
    // HKLM\Software\Classes\CLSID\{...}
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + pszCLSID;
    HKEY hKey = nullptr;
    LONG lr = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, clsidKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (lr != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lr);

    HRESULT hr = SetRegString(hKey, nullptr, tsf::Globals::text_service_description);
    RegCloseKey(hKey);
    if (FAILED(hr)) return hr;

    // HKLM\Software\Classes\CLSID\{...}\InprocServer32
    const std::wstring inprocKey = clsidKey + L"\\InprocServer32";
    lr = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, inprocKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (lr != ERROR_SUCCESS) return HRESULT_FROM_WIN32(lr);

    hr = SetRegString(hKey, nullptr, pszDllPath);
    if (SUCCEEDED(hr)) hr = SetRegString(hKey, L"ThreadingModel", L"Apartment");

    RegCloseKey(hKey);
    return hr;
}

/**
 * @brief Registers the TSF text service and its COM entries.
 */
HRESULT RegisterServer() {
    // Resolve the current DLL path.
    // std::wstring szDllPath(MAX_PATH, L'\0');
    wchar_t szDllPath[MAX_PATH] = {};
    if (GetModuleFileNameW(tsf::Globals::hinstance, szDllPath, MAX_PATH) == 0)
        return HRESULT_FROM_WIN32(GetLastError());

    // Convert CLSID to "{xxxxxxxx-...}" string form.
    wchar_t szCLSID[64] = {};
    if (StringFromGUID2(tsf::Globals::text_service_clsid, szCLSID, ARRAYSIZE(szCLSID)) == 0) return E_FAIL;

    // 1) Register COM in-process server.
    HRESULT hr = RegisterCOMServer(szCLSID, szDllPath);
    if (FAILED(hr)) return hr;

    // 2) Register TSF language profile.
    ITfInputProcessorProfiles* pProfiles = nullptr;
    hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER, IID_ITfInputProcessorProfiles,
                          reinterpret_cast<void**>(&pProfiles));
    if (FAILED(hr)) return hr;

    hr = pProfiles->Register(tsf::Globals::text_service_clsid);
    if (SUCCEEDED(hr)) {
        hr = pProfiles->AddLanguageProfile(
            tsf::Globals::text_service_clsid, tsf::Globals::tsf_language_id, tsf::Globals::text_service_profile_guid,
            tsf::Globals::text_service_description, static_cast<ULONG>(wcslen(tsf::Globals::text_service_description)),
            szDllPath,  // Icon file path (.ico).
            static_cast<ULONG>(wcslen(szDllPath)),
            0  // Icon index.
        );
    }
    pProfiles->Release();
    if (FAILED(hr)) return hr;

    // 3) Register category so Windows treats this TIP as a keyboard service.
    ITfCategoryMgr* pCategoryMgr = nullptr;
    hr = CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                          reinterpret_cast<void**>(&pCategoryMgr));
    if (SUCCEEDED(hr)) {
        hr = pCategoryMgr->RegisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_TIP_KEYBOARD, tsf::Globals::text_service_clsid);
    }
    if (SUCCEEDED(hr)) {
        hr = pCategoryMgr->RegisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, tsf::Globals::text_service_clsid);
    }
    // Required for the TIP to be listed in the Windows 8+ Settings UI.
    if (SUCCEEDED(hr)) {
        hr = pCategoryMgr->RegisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, tsf::Globals::text_service_clsid);
    }
    if (SUCCEEDED(hr)) {
        hr = pCategoryMgr->RegisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, tsf::Globals::text_service_clsid);
    }
    if (pCategoryMgr) {
        pCategoryMgr->Release();
    }

    return hr;
}

/**
 * @brief Unregisters the TSF text service and removes COM entries.
 */
HRESULT UnregisterServer() {
    wchar_t szCLSID[64] = {};
    if (StringFromGUID2(tsf::Globals::text_service_clsid, szCLSID, ARRAYSIZE(szCLSID)) == 0) return E_FAIL;

    // 1) Unregister TSF category.
    ITfCategoryMgr* pCategoryMgr = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_CategoryMgr, nullptr, CLSCTX_INPROC_SERVER, IID_ITfCategoryMgr,
                                   reinterpret_cast<void**>(&pCategoryMgr)))) {
        pCategoryMgr->UnregisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_TIP_KEYBOARD, tsf::Globals::text_service_clsid);
        pCategoryMgr->UnregisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_DISPLAYATTRIBUTEPROVIDER, tsf::Globals::text_service_clsid);
        pCategoryMgr->UnregisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_TIPCAP_IMMERSIVESUPPORT, tsf::Globals::text_service_clsid);
        pCategoryMgr->UnregisterCategory(
            tsf::Globals::text_service_clsid, GUID_TFCAT_TIPCAP_SYSTRAYSUPPORT, tsf::Globals::text_service_clsid);
        pCategoryMgr->Release();
    }

    // 2) Unregister TSF language profile.
    ITfInputProcessorProfiles* pProfiles = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_ITfInputProcessorProfiles, reinterpret_cast<void**>(&pProfiles)))) {
        pProfiles->RemoveLanguageProfile(
            tsf::Globals::text_service_clsid, tsf::Globals::tsf_language_id, tsf::Globals::text_service_profile_guid);
        pProfiles->Unregister(tsf::Globals::text_service_clsid);
        pProfiles->Release();
    }

    // 3) Remove COM registry tree.
    const std::wstring clsidKey = std::wstring(L"Software\\Classes\\CLSID\\") + szCLSID;
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, clsidKey.c_str());

    return S_OK;
}
