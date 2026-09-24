include_guard(GLOBAL)
include(FetchContent)

# Microsoft's experimental CMake integration, pinned to the package versions
# used by the official Windows App SDK CMake samples (including the Base fix).
FetchContent_Declare(NuGetCMakePackage
    GIT_REPOSITORY https://github.com/mschofie/NuGetCMakePackage
    GIT_TAG 5f547903030535424f913d1e8153fcd5f85997fa)
FetchContent_MakeAvailable(NuGetCMakePackage)

add_nuget_packages(
    CONFIG_FILE "${CMAKE_CURRENT_LIST_DIR}/nuget.config"
    LOCK_FILE "${CMAKE_CURRENT_LIST_DIR}/winui3.packages.lock.json"
    FRAMEWORK native
    PACKAGES
        Microsoft.Windows.CppWinRT 2.0.250303.1
        Microsoft.WindowsAppSDK.Base 2.0.5-experimental2
        Microsoft.WindowsAppSDK.Foundation 2.0.19-experimental
        Microsoft.WindowsAppSDK.InteractiveExperiences 2.0.11-experimental
        Microsoft.WindowsAppSDK.DWrite 2.0.2-experimental
        Microsoft.WindowsAppSDK.Runtime 2.0.0-experimental7
        Microsoft.Web.WebView2 1.0.3719.77
        Microsoft.WindowsAppSDK.WinUI 2.0.10-experimental)

find_package(Microsoft.WindowsAppSDK.Base CONFIG REQUIRED)
find_package(Microsoft.WindowsAppSDK.Foundation CONFIG REQUIRED)
find_package(Microsoft.WindowsAppSDK.InteractiveExperiences CONFIG REQUIRED)
find_package(Microsoft.WindowsAppSDK.DWrite CONFIG REQUIRED)
# The experimental WinUI config references WebView2 metadata but does not yet
# generate its headers. Use the SDK's projection helper for that dependency too.
get_property(webview2_dir GLOBAL PROPERTY NUGET_LOCATION-MICROSOFT_WEB_WEBVIEW2)
add_cppwinrt_projection(Microsoft.Web.WebView2
    INPUTS "${webview2_dir}/lib/Microsoft.Web.WebView2.Core.winmd"
    OPTIMIZE DEPS Microsoft.Windows.CppWinRT)
find_package(Microsoft.WindowsAppSDK.WinUI CONFIG REQUIRED)

function(llavon_target_winui3 target)
    target_link_libraries(${target} PRIVATE
        Microsoft.WindowsAppSDK.Foundation_SelfContained
        Microsoft.WindowsAppSDK.InteractiveExperiences_SelfContained
        Microsoft.WindowsAppSDK.DWrite_SelfContained
        Microsoft.WindowsAppSDK.WinUI_SelfContained)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND};-E;$<IF:$<BOOL:$<TARGET_RUNTIME_DLLS:${target}>>,copy_if_different;$<TARGET_RUNTIME_DLLS:${target}>;$<TARGET_FILE_DIR:${target}>,true>"
        COMMAND_EXPAND_LISTS VERBATIM)
    install(FILES $<TARGET_RUNTIME_DLLS:${target}>
        DESTINATION bin COMPONENT Runtime)
    # The experimental copy property covers root DLLs/PRI files only. Include
    # the native controls' assets and localized resources as well.
    wasdk_detect_platform()
    get_property(winui_dir GLOBAL PROPERTY NUGET_LOCATION-MICROSOFT_WINDOWSAPPSDK_WINUI)
    set(winui_runtime "${winui_dir}/runtimes-framework/win-${PLATFORM_IDENTIFIER}/native")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${winui_runtime}" "$<TARGET_FILE_DIR:${target}>"
        VERBATIM)
    install(DIRECTORY "${winui_runtime}/" DESTINATION bin COMPONENT Runtime)
    get_property(foundation_dir GLOBAL PROPERTY NUGET_LOCATION-MICROSOFT_WINDOWSAPPSDK_FOUNDATION)
    set(foundation_resources "${foundation_dir}/runtimes-framework/win-${PLATFORM_IDENTIFIER}/native/Microsoft.WindowsAppRuntime.pri")
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${foundation_resources}" "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    install(FILES "${foundation_resources}" DESTINATION bin COMPONENT Runtime)
    foreach(component BASE FOUNDATION INTERACTIVEEXPERIENCES DWRITE WINUI)
        get_property(package_dir GLOBAL PROPERTY NUGET_LOCATION-MICROSOFT_WINDOWSAPPSDK_${component})
        install(FILES "${package_dir}/license.txt" "${package_dir}/NOTICE.txt"
            DESTINATION "licenses/windowsappsdk/${component}" COMPONENT Runtime OPTIONAL)
    endforeach()
endfunction()
