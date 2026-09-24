#pragma once

#include <windows.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <string_view>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace llavon::settings {

inline winrt::Windows::Foundation::IInspectable load_xaml_resource(int id) {
    const auto module = reinterpret_cast<HINSTANCE>(&__ImageBase);
    const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) winrt::throw_last_error();
    const HGLOBAL loaded = LoadResource(module, resource);
    if (!loaded) winrt::throw_last_error();
    const auto* bytes = static_cast<const char*>(LockResource(loaded));
    const DWORD length = SizeofResource(module, resource);
    if (!bytes || length == 0) winrt::throw_hresult(E_FAIL);
    return winrt::Microsoft::UI::Xaml::Markup::XamlReader::Load(
        winrt::to_hstring(std::string_view(bytes, length)));
}

} // namespace llavon::settings
