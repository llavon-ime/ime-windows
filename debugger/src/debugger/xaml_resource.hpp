#pragma once

#include <windows.h>

#include <string_view>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/base.h>

namespace llavon::debugger {

inline winrt::Windows::Foundation::IInspectable load_xaml_resource(const HINSTANCE instance,
                                                                    const int id) {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!resource) winrt::throw_last_error();

    const HGLOBAL loaded = LoadResource(instance, resource);
    if (!loaded) winrt::throw_last_error();

    const auto* bytes = static_cast<const char*>(LockResource(loaded));
    const DWORD length = SizeofResource(instance, resource);
    if (!bytes || length == 0) winrt::throw_hresult(E_FAIL);

    return winrt::Windows::UI::Xaml::Markup::XamlReader::Load(
        winrt::to_hstring(std::string_view(bytes, length)));
}

}  // namespace llavon::debugger
