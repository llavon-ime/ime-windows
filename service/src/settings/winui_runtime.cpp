#include "winui_runtime.hpp"
#include "settings_resources.h"
#include "xaml_resource.hpp"

#include <windows.h>
#include <filesystem>
#include <string>
#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.Hosting.h>
#include <winrt/Microsoft.UI.Xaml.Markup.h>
#include <winrt/Microsoft.UI.Xaml.XamlTypeInfo.h>
#include <winrt/Microsoft.Windows.ApplicationModel.Resources.h>
#include <winrt/Windows.Foundation.Collections.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;
extern "C" HRESULT __stdcall WindowsAppRuntime_EnsureIsLoaded();

namespace llavon::settings {
namespace {
using namespace winrt::Microsoft::UI::Xaml;

std::filesystem::path module_directory() {
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        reinterpret_cast<HMODULE>(&__ImageBase), path.data(),
        static_cast<DWORD>(path.size()));
    if (!length || length == path.size()) winrt::throw_last_error();
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

void ensure_winui_modules_loaded() {
    // VitisAI sets the process-wide default DLL search directories. With that
    // policy, WinRT's manifest-relative activation fails with E_INVALIDARG.
    // Preload the settings page's resource, composition and input components by
    // absolute path, retaining them for the process-wide activation factory cache.
    [[maybe_unused]] static const bool loaded = [] {
        const auto directory = module_directory();
        for (const auto* name : {L"Microsoft.Windows.ApplicationModel.Resources.dll",
                L"dcompi.dll", L"Microsoft.UI.Input.dll", L"Microsoft.UI.Windowing.dll",
                L"Microsoft.Graphics.Display.dll", L"wuceffectsi.dll", L"WinUIEdit.dll"}) {
            const auto path = directory / name;
            if (!LoadLibraryExW(path.c_str(), nullptr,
                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)) {
                winrt::throw_last_error();
            }
        }
        return true;
    }();
}

struct IslandApplication : ApplicationT<IslandApplication, Markup::IXamlMetadataProvider> {
    IslandApplication() {
        // Loose settings XAML is embedded as RCDATA. Only the native control
        // library needs a PRI; resolve it explicitly without taking ownership
        // of the host executable's default resources.pri.
        ResourceManagerRequested([](const auto&, const ResourceManagerRequestedEventArgs& args) {
            const auto pri = module_directory() / L"Microsoft.UI.Xaml.Controls.pri";
            args.CustomResourceManager(
                winrt::Microsoft::Windows::ApplicationModel::Resources::ResourceManager(pri.c_str()));
        });
    }

    Markup::IXamlType GetXamlType(const winrt::Windows::UI::Xaml::Interop::TypeName& type) {
        return metadata_.GetXamlType(type);
    }
    Markup::IXamlType GetXamlType(const winrt::hstring& name) {
        return metadata_.GetXamlType(name);
    }
    winrt::com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() {
        return metadata_.GetXmlnsDefinitions();
    }

private:
    XamlTypeInfo::XamlControlsXamlMetaDataProvider metadata_;
};
}

struct WinuiRuntime::State {
    HANDLE activation_context = INVALID_HANDLE_VALUE;
    ULONG_PTR activation_cookie = 0;
    winrt::Microsoft::UI::Dispatching::DispatcherQueueController dispatcher{nullptr};
    Application application{nullptr};
    Hosting::WindowsXamlManager manager{nullptr};

    ~State() {
        // ShutdownQueue pumps pending work and raises the framework shutdown
        // notifications before the STA and its activation context disappear.
        try {
            if (manager) {
                manager.Close();
                manager = nullptr;
            }
            if (dispatcher) {
                dispatcher.ShutdownQueue();
                application = nullptr;
                dispatcher = nullptr;
            }
        } catch (...) {
            OutputDebugStringW(L"[settings-ui] WinUI dispatcher shutdown failed\n");
        }
        if (activation_cookie) DeactivateActCtx(0, activation_cookie);
        if (activation_context != INVALID_HANDLE_VALUE) ReleaseActCtx(activation_context);
    }
};

WinuiRuntime::WinuiRuntime() : state_(std::make_unique<State>()) {
    ACTCTXW context{sizeof(context)};
    context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
    context.hModule = reinterpret_cast<HMODULE>(&__ImageBase);
    context.lpResourceName = MAKEINTRESOURCEW(2);
    state_->activation_context = CreateActCtxW(&context);
    if (state_->activation_context == INVALID_HANDLE_VALUE) winrt::throw_last_error();
    if (!ActivateActCtx(state_->activation_context, &state_->activation_cookie)) {
        winrt::throw_last_error();
    }
    const wchar_t* stage = L"runtime activation";
    try {
        winrt::check_hresult(WindowsAppRuntime_EnsureIsLoaded());
        stage = L"WinUI dependency loading";
        ensure_winui_modules_loaded();
        stage = L"dispatcher queue";
        state_->dispatcher = winrt::Microsoft::UI::Dispatching::DispatcherQueueController::CreateOnCurrentThread();
        stage = L"XAML application";
        state_->application = winrt::make<IslandApplication>();
        stage = L"XAML manager";
        state_->manager = Hosting::WindowsXamlManager::InitializeForCurrentThread();
        stage = L"control resources";
        state_->application.Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
        state_->application.Resources().MergedDictionaries().Append(
            load_xaml_resource(IDR_SETTINGS_THEME_XAML).as<ResourceDictionary>());
    } catch (const winrt::hresult_error& error) {
        throw winrt::hresult_error(error.code(), winrt::hstring(stage) + L": " + error.message());
    }
}

WinuiRuntime::~WinuiRuntime() = default;

} // namespace llavon::settings
