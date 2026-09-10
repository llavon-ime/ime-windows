#pragma once

#include <windows.h>

#include <atomic>

namespace tsf {

struct Globals {
    /**
     * @brief DLL module handle
     */
    inline static HINSTANCE hinstance = nullptr;
    /**
     * @brief DLL reference count for managing lifetime
     */
    inline static std::atomic<LONG> dll_ref_count = 0;
    /**
     * @brief tsf language ID
     */
    inline constexpr static LANGID tsf_language_id = MAKELANGID(LANG_CHINESE, SUBLANG_CHINESE_TRADITIONAL);
    /**
     * @brief Text Service CLSID {C262415B-2D5F-4DA8-A7F5-0798802ECFAC}
     */
    inline constexpr static CLSID text_service_clsid = {
        0xc262415b, 0x2d5f, 0x4da8, {0xa7, 0xf5, 0x07, 0x98, 0x80, 0x2e, 0xcf, 0xac}};
    /**
     * @brief Text Service profile GUID {B2C3D4E5-F6A7-8901-BCDE-F01234567891}
     */
    inline constexpr static GUID text_service_profile_guid = {
        0xb2c3d4e5, 0xf6a7, 0x8901, {0xbc, 0xde, 0xf0, 0x12, 0x34, 0x56, 0x78, 0x91}};
    /**
     * @brief Text Service description name
     */
    inline constexpr static wchar_t text_service_description[] = L"拉風輸入法";
};

}  // namespace tsf
