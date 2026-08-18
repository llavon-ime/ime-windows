cmake_minimum_required(VERSION 3.24)

set(IME_WINDOWS_SERVICE_VERSION "0.1.0" CACHE STRING "Llavon IME package version")
set(IME_WINDOWS_SERVICE_BUILD_COMMIT "unknown" CACHE STRING
    "Root ime-windows repository commit embedded in the service UI")
set(IME_WINDOWS_SERVICE_BUILD_NUMBER "0" CACHE STRING
    "CI build number embedded in the service UI")
set(LLAVON_DEBUG_CLIENT_DIR "" CACHE PATH
    "Directory containing the installed llavon-debug-client CMake package")
if(NOT IME_WINDOWS_SERVICE_BUILD_NUMBER MATCHES "^[0-9]+$")
    message(FATAL_ERROR "IME_WINDOWS_SERVICE_BUILD_NUMBER must be a non-negative integer")
endif()

if(NOT DEFINED IME_WINDOWS_SERVICE_SUPERBUILD)
    set(IME_WINDOWS_SERVICE_SUPERBUILD ON CACHE BOOL "Build ime-core before the Windows service")
endif()

if(IME_WINDOWS_SERVICE_SUPERBUILD)
    # Each child has its own configure step and manifest. Avoid creating an
    # unused third vcpkg tree for this orchestration-only configure.
    set(VCPKG_MANIFEST_INSTALL OFF CACHE BOOL "Install superbuild dependencies" FORCE)

    project(ime_windows_service VERSION ${IME_WINDOWS_SERVICE_VERSION} LANGUAGES NONE)

    include(ExternalProject)

    if(NOT WIN32)
        message(FATAL_ERROR "ime-windows-service is Windows-only")
    endif()
    if(NOT CMAKE_TOOLCHAIN_FILE)
        message(FATAL_ERROR "ime-windows-service requires CMAKE_TOOLCHAIN_FILE")
    endif()
    if(NOT LLAVON_DEBUG_CLIENT_DIR)
        message(FATAL_ERROR "LLAVON_DEBUG_CLIENT_DIR is required")
    endif()

    set(IME_WINDOWS_SERVICE_CONFIGURATION
        "Release"
        CACHE STRING
        "Configuration used by child builds"
    )
    set_property(
        CACHE IME_WINDOWS_SERVICE_CONFIGURATION
        PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel
    )
    set(IME_WINDOWS_SERVICE_VCPKG_TRIPLET
        "x64-windows"
        CACHE STRING
        "vcpkg triplet used by ime-core and the Windows service"
    )
    set(IME_WINDOWS_SERVICE_CORE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/ime-core"
        CACHE PATH
        "ime-core submodule source directory"
    )

    if(NOT EXISTS "${IME_WINDOWS_SERVICE_CORE_SOURCE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "ime-core was not found at ${IME_WINDOWS_SERVICE_CORE_SOURCE_DIR}. "
            "Initialize the ime-core submodule."
        )
    endif()

    set(_ime_core_binary_dir "${CMAKE_CURRENT_BINARY_DIR}/ime-core-build")
    set(_ime_core_install_dir "${CMAKE_CURRENT_BINARY_DIR}/ime-core-install")
    set(_ime_service_binary_dir "${CMAKE_CURRENT_BINARY_DIR}/service-build")

    ExternalProject_Add(ime-core-build
        SOURCE_DIR "${IME_WINDOWS_SERVICE_CORE_SOURCE_DIR}"
        BINARY_DIR "${_ime_core_binary_dir}"
        INSTALL_DIR "${_ime_core_install_dir}"
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE:STRING=${IME_WINDOWS_SERVICE_CONFIGURATION}"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DVCPKG_MANIFEST_INSTALL:BOOL=ON"
            "-DVCPKG_TARGET_TRIPLET:STRING=${IME_WINDOWS_SERVICE_VCPKG_TRIPLET}"
            "-DVCPKG_OVERLAY_TRIPLETS:PATH=${VCPKG_OVERLAY_TRIPLETS}"
            "-DVCPKG_MANIFEST_FEATURES:STRING=llama-cuda|llama-vulkan"
        LIST_SEPARATOR "|"
        BUILD_COMMAND
            "${CMAKE_COMMAND}"
                --build "<BINARY_DIR>"
                --config "${IME_WINDOWS_SERVICE_CONFIGURATION}"
        INSTALL_COMMAND
            "${CMAKE_COMMAND}"
                --install "<BINARY_DIR>"
                --config "${IME_WINDOWS_SERVICE_CONFIGURATION}"
        BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE
    )

    ExternalProject_Add(ime-windows-service-build
        DEPENDS ime-core-build
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
        BINARY_DIR "${_ime_service_binary_dir}"
        INSTALL_DIR "${CMAKE_INSTALL_PREFIX}"
        CMAKE_ARGS
            "-DIME_WINDOWS_SERVICE_SUPERBUILD:BOOL=OFF"
            "-DIME_WINDOWS_SERVICE_VERSION:STRING=${IME_WINDOWS_SERVICE_VERSION}"
            "-DIME_WINDOWS_SERVICE_BUILD_COMMIT:STRING=${IME_WINDOWS_SERVICE_BUILD_COMMIT}"
            "-DIME_WINDOWS_SERVICE_BUILD_NUMBER:STRING=${IME_WINDOWS_SERVICE_BUILD_NUMBER}"
            "-DCMAKE_BUILD_TYPE:STRING=${IME_WINDOWS_SERVICE_CONFIGURATION}"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${CMAKE_TOOLCHAIN_FILE}"
            "-DVCPKG_MANIFEST_DIR:PATH=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DVCPKG_MANIFEST_INSTALL:BOOL=ON"
            "-DVCPKG_TARGET_TRIPLET:STRING=${IME_WINDOWS_SERVICE_VCPKG_TRIPLET}"
            "-DVCPKG_OVERLAY_TRIPLETS:PATH=${VCPKG_OVERLAY_TRIPLETS}"
            "-Dime-core_DIR:PATH=${_ime_core_install_dir}/lib/cmake/ime-core"
            "-Dllavon-debug-client_DIR:PATH=${LLAVON_DEBUG_CLIENT_DIR}"
            "-DIME_CORE_VCPKG_PREFIX:PATH=${_ime_core_binary_dir}/vcpkg_installed/${IME_WINDOWS_SERVICE_VCPKG_TRIPLET}"
        BUILD_COMMAND
            "${CMAKE_COMMAND}"
                --build "<BINARY_DIR>"
                --config "${IME_WINDOWS_SERVICE_CONFIGURATION}"
        INSTALL_COMMAND
            "${CMAKE_COMMAND}"
                --install "<BINARY_DIR>"
                --config "${IME_WINDOWS_SERVICE_CONFIGURATION}"
        BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE
    )

    add_custom_target(ime-windows-service-artifacts ALL
        DEPENDS ime-windows-service-build
    )

    install(DIRECTORY
        "${_ime_core_binary_dir}/vcpkg_installed/${IME_WINDOWS_SERVICE_VCPKG_TRIPLET}/bin/"
        DESTINATION bin
        FILES_MATCHING PATTERN "*.dll"
    )
    install(DIRECTORY "${_ime_core_install_dir}/share/ime-core/tables/"
        DESTINATION tables
    )
    install(FILES LICENSE DESTINATION licenses COMPONENT Runtime)
    install(FILES "${IME_WINDOWS_SERVICE_CORE_SOURCE_DIR}/LICENSE"
        DESTINATION licenses
        RENAME ime-core-LICENSE.txt
        COMPONENT Runtime
    )

    return()
endif()

project(ime_windows_service VERSION ${IME_WINDOWS_SERVICE_VERSION} LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "ime-windows-service is Windows-only")
endif()

set(IME_CORE_VCPKG_PREFIX
    ""
    CACHE PATH
    "vcpkg installed prefix used by the separately configured ime-core project"
)
if(NOT IME_CORE_VCPKG_PREFIX)
    message(FATAL_ERROR "IME_CORE_VCPKG_PREFIX is required")
endif()
list(PREPEND CMAKE_PREFIX_PATH "${IME_CORE_VCPKG_PREFIX}")

find_package(asio CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
find_package(ime-core CONFIG REQUIRED)
find_package(llavon-debug-client CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)

include(CTest)

add_executable(llavon-ime-service
    src/assets/llavon_tray.jpg
    src/assets/llavon_tray.ico
    src/main.cpp
    src/resource.h
    src/service/candidate_pipe_server.hpp
    src/service/candidate_pipe_protocol.hpp
    src/service/candidate_ui_loader.cpp
    src/service/candidate_ui_loader.hpp
    src/service/debug/core_logger_adapter.hpp
    src/service/inference_settings.cpp
    src/service/inference_settings.hpp
    src/service/inference_settings_codec.cpp
    src/service/inference_settings_codec.hpp
    src/service/prediction_pipe_server.hpp
    src/service/settings_ui_loader.cpp
    src/service/settings_ui_loader.hpp
    src/service/tray_icon.cpp
    src/service/tray_icon.hpp
    src/service.rc
)

add_library(llavon-ime-candidate-ui SHARED
    src/candidate/candidate_log.hpp
    src/candidate/candidate_runtime.cpp
    src/candidate/candidate_ui_api.h
    src/candidate/candidate_window.hpp
    src/candidate/window.hpp
)

add_library(llavon-ime-settings-ui SHARED
    src/settings/settings_configuration.hpp
    src/settings/settings_runtime.cpp
    src/settings/settings_ui_api.h
    src/settings/settings_window.cpp
    src/settings/settings_window.hpp
    src/settings/update_checker.cpp
    src/settings/update_checker.hpp
)

target_compile_features(llavon-ime-service PRIVATE cxx_std_23)
target_compile_features(llavon-ime-candidate-ui PRIVATE cxx_std_23)
target_compile_features(llavon-ime-settings-ui PRIVATE cxx_std_23)
set_target_properties(llavon-ime-service PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
set_target_properties(llavon-ime-settings-ui PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    OUTPUT_NAME "llavon-ime-settings-ui"
)
set_target_properties(llavon-ime-candidate-ui PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    OUTPUT_NAME "llavon-ime-candidate-ui"
)
target_compile_definitions(llavon-ime-service PRIVATE
    LLAVON_IME_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(llavon-ime-settings-ui PRIVATE
    LLAVON_IME_BUILD_COMMIT="${IME_WINDOWS_SERVICE_BUILD_COMMIT}"
    LLAVON_IME_BUILD_NUMBER=${IME_WINDOWS_SERVICE_BUILD_NUMBER}
    LLAVON_SETTINGS_UI_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(llavon-ime-candidate-ui PRIVATE
    LLAVON_CANDIDATE_UI_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_options(llavon-ime-service PRIVATE
    /W4
    /WX
    /utf-8
    /await:strict
)
target_compile_options(llavon-ime-settings-ui PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)
target_compile_options(llavon-ime-candidate-ui PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)
target_link_options(llavon-ime-service PRIVATE /MANIFEST:NO)
target_include_directories(llavon-ime-service PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(llavon-ime-settings-ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings"
)
target_include_directories(llavon-ime-candidate-ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/candidate"
)
target_link_libraries(llavon-ime-service PRIVATE
    ime-core::ime-core
    llavon::debug-client
    asio::asio
    reflectcpp::reflectcpp
    advapi32
    ole32
    oleaut32
    shell32
    ws2_32
)
target_link_libraries(llavon-ime-settings-ui PRIVATE
    Microsoft::CppWinRT
    WindowsApp
    comctl32
    dwmapi
    ole32
    runtimeobject
)
target_link_libraries(llavon-ime-candidate-ui PRIVATE
    Microsoft::CppWinRT
    WindowsApp
    ole32
    runtimeobject
)

if(BUILD_TESTING)
    add_executable(candidate-pipe-protocol-tests
        tests/candidate_pipe_protocol_tests.cpp
        src/service/candidate_pipe_protocol.hpp
    )
    target_compile_features(candidate-pipe-protocol-tests PRIVATE cxx_std_23)
    target_compile_definitions(candidate-pipe-protocol-tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_compile_options(candidate-pipe-protocol-tests PRIVATE
        /W4
        /WX
        /utf-8
        /permissive-
    )
    target_include_directories(candidate-pipe-protocol-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME candidate-pipe-protocol COMMAND candidate-pipe-protocol-tests)

    add_executable(inference-settings-tests
        tests/inference_settings_tests.cpp
        src/service/inference_settings_codec.cpp
        src/service/inference_settings_codec.hpp
    )
    target_compile_features(inference-settings-tests PRIVATE cxx_std_23)
    target_compile_definitions(inference-settings-tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_compile_options(inference-settings-tests PRIVATE
        /W4
        /WX
        /utf-8
        /permissive-
    )
    target_include_directories(inference-settings-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(inference-settings-tests PRIVATE
        ime-core::ime-core
        reflectcpp::reflectcpp
    )
    add_test(NAME inference-settings COMMAND inference-settings-tests)
endif()

install(TARGETS
    llavon-ime-service
    llavon-ime-candidate-ui
    llavon-ime-settings-ui
    RUNTIME DESTINATION bin
)
