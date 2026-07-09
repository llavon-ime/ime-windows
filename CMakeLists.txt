cmake_minimum_required(VERSION 3.24)

project(llavon_ime_windows LANGUAGES NONE)

include(ExternalProject)

set(LLAVON_IME_VERSION "0.1.0" CACHE STRING "Package version")

set(LLAVON_IME_CONFIGURATION "Release" CACHE STRING "Configuration used by child builds")
set_property(CACHE LLAVON_IME_CONFIGURATION PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)

set(LLAVON_IME_ARTIFACT_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/dist"
    CACHE PATH
    "Directory where child project install outputs are collected"
)

set(LLAVON_IME_VCPKG_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg"
    CACHE PATH
    "vcpkg root used to configure child projects"
)

set(LLAVON_IME_CMAKE_TOOLCHAIN_FILE
    "${LLAVON_IME_VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
    CACHE FILEPATH
    "CMake toolchain file passed to child projects"
)

set(LLAVON_IME_MODEL_URL
    "https://huggingface.co/tony65535/llavon-ime-llama-250m-GGUF/resolve/main/llavon-ime-llama-250m-Q4_K_M.gguf?download=true"
    CACHE STRING
    "Model URL packaged with the installer"
)

set(LLAVON_IME_WIX_VERSION "4.0.4" CACHE STRING "WiX .NET tool version used by CPack")
set(LLAVON_IME_WIX_UI_EXTENSION_VERSION "4.0.4" CACHE STRING "WiX UI extension version used by CPack")
set(LLAVON_IME_WIX_TOOLS_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/.wix-tools"
    CACHE PATH
    "Directory where package-only WiX NuGet tools are installed"
)
set(LLAVON_IME_WIX_EXTENSIONS_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/.wix-extensions"
    CACHE PATH
    "Directory where package-only WiX extensions are installed"
)

set(LLAVON_IME_MODEL_FILENAME "llavon-ime-llama-250m-Q4_K_M.gguf")
set(LLAVON_IME_SERVICE_STARTUP_LAUNCHER
    "${CMAKE_CURRENT_BINARY_DIR}/start-llavon-ime-service.vbs"
)
set(LLAVON_IME_DOWNLOAD_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/downloads"
    CACHE PATH
    "Directory used to cache downloaded package inputs"
)
set(LLAVON_IME_MODEL_PATH "${LLAVON_IME_DOWNLOAD_DIR}/${LLAVON_IME_MODEL_FILENAME}")
set(LLAVON_IME_WIX_EXE "${LLAVON_IME_WIX_TOOLS_DIR}/wix.cmd")
set(LLAVON_IME_WIX_MARKER "${LLAVON_IME_WIX_EXTENSIONS_DIR}/WixToolset.UI.wixext-${LLAVON_IME_WIX_UI_EXTENSION_VERSION}.stamp")

if(NOT EXISTS "${LLAVON_IME_CMAKE_TOOLCHAIN_FILE}")
    message(FATAL_ERROR
        "vcpkg toolchain file was not found: ${LLAVON_IME_CMAKE_TOOLCHAIN_FILE}\n"
        "Initialize the vcpkg submodule or pass "
        "-DLLAVON_IME_CMAKE_TOOLCHAIN_FILE=<path-to-vcpkg.cmake>."
    )
endif()

file(GENERATE
    OUTPUT "${LLAVON_IME_SERVICE_STARTUP_LAUNCHER}"
    CONTENT "Set fso = CreateObject(\"Scripting.FileSystemObject\")\r\nSet shell = CreateObject(\"WScript.Shell\")\r\nbinDir = fso.GetParentFolderName(WScript.ScriptFullName)\r\nexePath = fso.BuildPath(binDir, \"llavon-ime-service.exe\")\r\nshell.Run Chr(34) & exePath & Chr(34), 0, False\r\n"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
    COPYONLY
)

function(llavon_add_child_project name source_dir install_subdir)
    set(binary_dir "${CMAKE_CURRENT_BINARY_DIR}/${name}")
    set(install_dir "${LLAVON_IME_ARTIFACT_DIR}/${install_subdir}")

    ExternalProject_Add(${name}
        SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${source_dir}"
        BINARY_DIR "${binary_dir}"
        INSTALL_DIR "${install_dir}"
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE:STRING=${LLAVON_IME_CONFIGURATION}"
            "-DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR>"
            "-DCMAKE_TOOLCHAIN_FILE:FILEPATH=${LLAVON_IME_CMAKE_TOOLCHAIN_FILE}"
        BUILD_COMMAND
            "${CMAKE_COMMAND}"
                --build "<BINARY_DIR>"
                --config "${LLAVON_IME_CONFIGURATION}"
        INSTALL_COMMAND
            "${CMAKE_COMMAND}"
                --install "<BINARY_DIR>"
                --config "${LLAVON_IME_CONFIGURATION}"
        BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE
        USES_TERMINAL_BUILD TRUE
        USES_TERMINAL_INSTALL TRUE
    )
endfunction()

llavon_add_child_project(llavon-ime-service-build ime-service ime-service)
llavon_add_child_project(llavon-ime-frontend-build ime-windows-frontend ime-windows-frontend)

add_custom_target(llavon-ime-artifacts
    DEPENDS
        llavon-ime-service-build
        llavon-ime-frontend-build
)

add_custom_command(
    OUTPUT "${LLAVON_IME_MODEL_PATH}"
    COMMAND
        "${CMAKE_COMMAND}"
            "-DLLAVON_IME_MODEL_URL=${LLAVON_IME_MODEL_URL}"
            "-DLLAVON_IME_MODEL_PATH=${LLAVON_IME_MODEL_PATH}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DownloadModel.cmake"
    COMMENT "Downloading ${LLAVON_IME_MODEL_FILENAME}"
    VERBATIM
)

add_custom_target(llavon-ime-model DEPENDS "${LLAVON_IME_MODEL_PATH}")

add_custom_command(
    OUTPUT
        "${LLAVON_IME_WIX_EXE}"
        "${LLAVON_IME_WIX_MARKER}"
    COMMAND
        "${CMAKE_COMMAND}"
            "-DLLAVON_IME_WIX_VERSION=${LLAVON_IME_WIX_VERSION}"
            "-DLLAVON_IME_WIX_UI_EXTENSION_VERSION=${LLAVON_IME_WIX_UI_EXTENSION_VERSION}"
            "-DLLAVON_IME_WIX_TOOLS_DIR=${LLAVON_IME_WIX_TOOLS_DIR}"
            "-DLLAVON_IME_WIX_EXTENSIONS_DIR=${LLAVON_IME_WIX_EXTENSIONS_DIR}"
            "-DLLAVON_IME_WIX_EXE=${LLAVON_IME_WIX_EXE}"
            "-DLLAVON_IME_WIX_MARKER=${LLAVON_IME_WIX_MARKER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureWix.cmake"
    COMMENT "Restoring WiX ${LLAVON_IME_WIX_VERSION} from NuGet"
    VERBATIM
)

add_custom_target(llavon-ime-wix
    DEPENDS
        "${LLAVON_IME_WIX_EXE}"
        "${LLAVON_IME_WIX_MARKER}"
)

add_custom_target(llavon-ime-package-inputs
    DEPENDS
        llavon-ime-artifacts
        llavon-ime-model
        llavon-ime-wix
)

add_custom_target(llavon-ime-msi
    COMMAND
        "${CMAKE_COMMAND}" -E env
            "WIX_EXTENSIONS=${LLAVON_IME_WIX_EXTENSIONS_DIR}"
            "${CMAKE_CPACK_COMMAND}"
            -C "${LLAVON_IME_CONFIGURATION}"
            --config "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS llavon-ime-package-inputs
    USES_TERMINAL
)

install(DIRECTORY "${LLAVON_IME_ARTIFACT_DIR}/ime-service/bin/"
    DESTINATION bin
    COMPONENT Runtime
)

install(DIRECTORY "${LLAVON_IME_ARTIFACT_DIR}/ime-windows-frontend/bin/"
    DESTINATION bin
    COMPONENT Runtime
)

install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ime-service/table/tokens"
    DESTINATION tables
    COMPONENT Runtime
)

install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ime-windows-frontend/table/bopomofo_char.json"
    DESTINATION tables
    COMPONENT Runtime
)

install(FILES "${LLAVON_IME_MODEL_PATH}"
    DESTINATION models
    COMPONENT Runtime
)

install(PROGRAMS "${LLAVON_IME_SERVICE_STARTUP_LAUNCHER}"
    DESTINATION bin
    COMPONENT Runtime
)

set(CPACK_GENERATOR "WIX")
set(CPACK_PACKAGE_NAME "Llavon IME")
set(CPACK_PACKAGE_VENDOR "Llavon IME")
set(CPACK_PACKAGE_VERSION "${LLAVON_IME_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "0")
set(CPACK_PACKAGE_VERSION_MINOR "1")
set(CPACK_PACKAGE_VERSION_PATCH "0")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Llavon IME for Windows")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Llavon IME")
set(CPACK_PACKAGING_INSTALL_PREFIX "")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt")
set(CPACK_PACKAGE_FILE_NAME "llavon-ime-${LLAVON_IME_VERSION}-windows")
set(CPACK_WIX_VERSION "4")
set(CPACK_WIX_EXTENSIONS "WixToolset.UI.wixext")
set(CPACK_WIX_ROOT "${LLAVON_IME_WIX_TOOLS_DIR}")
set(CPACK_WIX_EXECUTABLE "${LLAVON_IME_WIX_EXE}")
set(CPACK_WIX_SIZEOF_VOID_P "8")
set(CPACK_WIX_ARCHITECTURE "x64")
set(CPACK_WIX_INSTALL_SCOPE "perMachine")
set(CPACK_WIX_UPGRADE_GUID "FD29FA2D-C02C-47CB-AA80-B334BACAABF0")
set(CPACK_WIX_UI_REF "WixUI_InstallDir")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Llavon IME")
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/RegisterIme.wxs")
set(CPACK_WIX_EXTRA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/Autostart.wxs")
set(CPACK_WIX_PROPERTY_ARPCOMMENTS "Llavon IME Windows installer")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/llavon-ime")

include(CPack)

if(TARGET package)
    add_dependencies(package llavon-ime-package-inputs)
endif()

if(TARGET PACKAGE)
    add_dependencies(PACKAGE llavon-ime-package-inputs)
endif()
