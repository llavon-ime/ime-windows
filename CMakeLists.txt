cmake_minimum_required(VERSION 3.31)

# The installer version is independent of release tags. WiX replaces
# same-version bundles, so local and published installers can swap.
set(LLAVON_IME_VERSION "0.0.1" CACHE STRING "Fixed installer version" FORCE)
set(LLAVON_IME_MSI_VERSION "0.0.1" CACHE STRING "Fixed MSI product version" FORCE)
option(LLAVON_IME_ENABLE_CUDA "Build ime-core with the CUDA backend" OFF)

# vcpkg's toolchain is loaded by project(). Select optional manifest features
# before that happens so one configure owns the complete dependency graph.
if(LLAVON_IME_ENABLE_CUDA)
    list(APPEND VCPKG_MANIFEST_FEATURES "llama-cuda")
    list(REMOVE_DUPLICATES VCPKG_MANIFEST_FEATURES)
endif()

project(llavon_ime_windows
    VERSION "${LLAVON_IME_MSI_VERSION}"
    LANGUAGES CXX RC
)

if(NOT WIN32)
    message(FATAL_ERROR "Llavon IME for Windows is Windows-only")
endif()

include(CTest)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin")
foreach(_configuration IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_configuration} "${CMAKE_CURRENT_BINARY_DIR}/bin")
endforeach()

set(LLAVON_IME_BUILD_COMMIT "" CACHE STRING "Root repository commit embedded in the package")
set(LLAVON_IME_BUILD_NUMBER "" CACHE STRING "CI build number embedded in the package")
if(NOT LLAVON_IME_BUILD_NUMBER)
    if(DEFINED ENV{GITHUB_RUN_NUMBER} AND "$ENV{GITHUB_RUN_NUMBER}" MATCHES "^[0-9]+$")
        set(_llavon_ime_build_number "$ENV{GITHUB_RUN_NUMBER}")
    else()
        set(_llavon_ime_build_number "0")
    endif()
    set(LLAVON_IME_BUILD_NUMBER "${_llavon_ime_build_number}" CACHE STRING
        "CI build number embedded in the package" FORCE)
endif()
if(NOT LLAVON_IME_BUILD_NUMBER MATCHES "^[0-9]+$")
    message(FATAL_ERROR "LLAVON_IME_BUILD_NUMBER must be a non-negative integer")
endif()

if(NOT LLAVON_IME_BUILD_COMMIT)
    string(LENGTH "$ENV{GITHUB_SHA}" _llavon_ime_github_sha_length)
    if(DEFINED ENV{GITHUB_SHA} AND "$ENV{GITHUB_SHA}" MATCHES "^[0-9a-fA-F]+$" AND
       _llavon_ime_github_sha_length GREATER_EQUAL 40 AND
       _llavon_ime_github_sha_length LESS_EQUAL 64)
        set(_llavon_ime_build_commit "$ENV{GITHUB_SHA}")
    else()
        execute_process(
            COMMAND git rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE _llavon_ime_git_result
            OUTPUT_VARIABLE _llavon_ime_build_commit
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
        )
        string(LENGTH "${_llavon_ime_build_commit}" _llavon_ime_git_sha_length)
        if(NOT _llavon_ime_git_result EQUAL 0 OR
           NOT _llavon_ime_build_commit MATCHES "^[0-9a-fA-F]+$" OR
           _llavon_ime_git_sha_length LESS 40 OR _llavon_ime_git_sha_length GREATER 64)
            set(_llavon_ime_build_commit "unknown")
        endif()
    endif()
    set(LLAVON_IME_BUILD_COMMIT "${_llavon_ime_build_commit}" CACHE STRING
        "Root repository commit embedded in the package" FORCE)
endif()

set(IME_WINDOWS_SERVICE_BUILD_COMMIT "${LLAVON_IME_BUILD_COMMIT}")
set(IME_WINDOWS_SERVICE_BUILD_NUMBER "${LLAVON_IME_BUILD_NUMBER}")

# The gitlink is the trainer revision shipped by this IME source tree.
execute_process(
    COMMAND git rev-parse HEAD:lora-trainer
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE _llavon_lora_git_result
    OUTPUT_VARIABLE LLAVON_LORA_SUBMODULE_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
string(LENGTH "${LLAVON_LORA_SUBMODULE_COMMIT}" _llavon_lora_commit_length)
if(NOT _llavon_lora_git_result EQUAL 0 OR
   NOT LLAVON_LORA_SUBMODULE_COMMIT MATCHES "^[0-9a-fA-F]+$" OR
   NOT _llavon_lora_commit_length EQUAL 40)
    set(LLAVON_LORA_SUBMODULE_COMMIT "unknown")
endif()

set(LLAVON_IME_MODEL_REPO
    "tony65535/llavon-ime-llama-250m-GGUF"
    CACHE STRING
    "Hugging Face repository containing the web setup model"
)
set(LLAVON_IME_MODEL_REVISION
    ""
    CACHE STRING
    "Optional pre-resolved Hugging Face model revision"
)
set(LLAVON_IME_MODEL_FILENAME "llavon-ime-llama-250m-Q4_K_M.gguf")
set(LLAVON_IME_MODEL_SHA256 "" CACHE STRING "Pinned GGUF SHA-256 for the web setup")
set(LLAVON_IME_MODEL_SIZE "" CACHE STRING "Pinned GGUF size for the web setup")

set(LLAVON_IME_WIX_VERSION "7.0.0" CACHE STRING "WiX .NET tool version used by CPack")
set(LLAVON_IME_WIX_UI_EXTENSION_VERSION "7.0.0" CACHE STRING "WiX UI extension version used by CPack")
set(LLAVON_IME_WIX_BAL_EXTENSION_VERSION "7.0.0" CACHE STRING "WiX bootstrapper extension version")
option(LLAVON_IME_WIX_ACCEPT_EULA "Accept the WiX 7 EULA for packaging" OFF)
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
set(LLAVON_IME_WIX_EXE "${LLAVON_IME_WIX_TOOLS_DIR}/wix.cmd")
set(LLAVON_IME_WIX_MARKER
    "${LLAVON_IME_WIX_EXTENSIONS_DIR}/WixToolset.UI.wixext-${LLAVON_IME_WIX_UI_EXTENSION_VERSION}.stamp"
)
set(LLAVON_IME_WIX_BAL_MARKER
    "${LLAVON_IME_WIX_EXTENSIONS_DIR}/WixToolset.BootstrapperApplications.wixext-${LLAVON_IME_WIX_BAL_EXTENSION_VERSION}.stamp"
)

# The service resolves the matching release from this pinned gitlink commit.
# No LoRA release lookup or trainer download runs while building the IME.
if(LLAVON_LORA_SUBMODULE_COMMIT STREQUAL "unknown")
    message(FATAL_ERROR "Unable to identify the pinned LoRA Trainer submodule commit")
endif()

set(LLAVON_IME_SERVICE_STARTUP_LAUNCHER
    "${CMAKE_CURRENT_BINARY_DIR}/start-llavon-ime-service.vbs"
)
file(GENERATE
    OUTPUT "${LLAVON_IME_SERVICE_STARTUP_LAUNCHER}"
    CONTENT "Set fso = CreateObject(\"Scripting.FileSystemObject\")\r\nSet shell = CreateObject(\"WScript.Shell\")\r\nbinDir = fso.GetParentFolderName(WScript.ScriptFullName)\r\nexePath = fso.BuildPath(binDir, \"llavon-ime-service.exe\")\r\nshell.Run Chr(34) & exePath & Chr(34), 0, False\r\n"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
    COPYONLY
)
set(LLAVON_IME_PACKAGE_LICENSE "${CMAKE_CURRENT_BINARY_DIR}/PACKAGE-LICENSE.txt")
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" LLAVON_IME_SOFTWARE_LICENSE_TEXT)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/MODEL-LICENSE.txt" LLAVON_IME_MODEL_LICENSE_TEXT)
file(WRITE "${LLAVON_IME_PACKAGE_LICENSE}"
    "LLAVON IME SOFTWARE LICENSE\n"
    "===========================\n\n"
    "${LLAVON_IME_SOFTWARE_LICENSE_TEXT}\n\n"
    "LLAVON IME MODEL LICENSE\n"
    "========================\n\n"
    "${LLAVON_IME_MODEL_LICENSE_TEXT}"
)

# ime-core remains its own repository, but it is a normal static-library target
# in this build. EXCLUDE_FROM_ALL suppresses its standalone install/export rules;
# the service dependency still builds the library.
if(NOT VCPKG_TARGET_TRIPLET OR NOT VCPKG_INSTALLED_DIR)
    message(FATAL_ERROR "Configure with the repository's vcpkg toolchain")
endif()

add_subdirectory(ime-core EXCLUDE_FROM_ALL)
add_subdirectory(debugger)
add_subdirectory(service)
add_subdirectory(frontend)

add_custom_target(llavon-ime-artifacts
    DEPENDS
        llavon-ime
        llavon-ime-lifecycle
        llavon-ime-service
        llavon-ime-settings-ui
        llavon-ime-candidate-ui
        llavon-ime-debugger
)

set(LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/licenses/vcpkg")
set(LLAVON_IME_VCPKG_LICENSE_AGGREGATE
    "${CMAKE_CURRENT_BINARY_DIR}/licenses/THIRD-PARTY-vcpkg-LICENSES.txt")
set(LLAVON_IME_VCPKG_LICENSE_MARKER "${CMAKE_CURRENT_BINARY_DIR}/licenses/vcpkg-licenses.stamp")
add_custom_command(
    OUTPUT
        "${LLAVON_IME_VCPKG_LICENSE_AGGREGATE}"
        "${LLAVON_IME_VCPKG_LICENSE_MARKER}"
    COMMAND
        "${CMAKE_COMMAND}"
            "-DLLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR=${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}"
            "-DLLAVON_IME_VCPKG_LICENSE_AGGREGATE=${LLAVON_IME_VCPKG_LICENSE_AGGREGATE}"
            "-DLLAVON_IME_VCPKG_LICENSE_MARKER=${LLAVON_IME_VCPKG_LICENSE_MARKER}"
            "-DVCPKG_TARGET_TRIPLET=${VCPKG_TARGET_TRIPLET}"
            "-DVCPKG_INSTALLED_DIR=${VCPKG_INSTALLED_DIR}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CollectVcpkgLicenses.cmake"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CollectVcpkgLicenses.cmake"
        "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg.json"
    COMMENT "Collecting vcpkg package licenses"
    VERBATIM
)
add_custom_target(llavon-ime-vcpkg-licenses
    DEPENDS
        "${LLAVON_IME_VCPKG_LICENSE_AGGREGATE}"
        "${LLAVON_IME_VCPKG_LICENSE_MARKER}"
)

add_custom_command(
    OUTPUT
        "${LLAVON_IME_WIX_EXE}"
        "${LLAVON_IME_WIX_MARKER}"
        "${LLAVON_IME_WIX_BAL_MARKER}"
    COMMAND
        "${CMAKE_COMMAND}"
            "-DLLAVON_IME_WIX_VERSION=${LLAVON_IME_WIX_VERSION}"
            "-DLLAVON_IME_WIX_UI_EXTENSION_VERSION=${LLAVON_IME_WIX_UI_EXTENSION_VERSION}"
            "-DLLAVON_IME_WIX_BAL_EXTENSION_VERSION=${LLAVON_IME_WIX_BAL_EXTENSION_VERSION}"
            "-DLLAVON_IME_WIX_ACCEPT_EULA=${LLAVON_IME_WIX_ACCEPT_EULA}"
            "-DLLAVON_IME_WIX_TOOLS_DIR=${LLAVON_IME_WIX_TOOLS_DIR}"
            "-DLLAVON_IME_WIX_EXTENSIONS_DIR=${LLAVON_IME_WIX_EXTENSIONS_DIR}"
            "-DLLAVON_IME_WIX_EXE=${LLAVON_IME_WIX_EXE}"
            "-DLLAVON_IME_WIX_MARKER=${LLAVON_IME_WIX_MARKER}"
            "-DLLAVON_IME_WIX_BAL_MARKER=${LLAVON_IME_WIX_BAL_MARKER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureWix.cmake"
    COMMENT "Restoring WiX ${LLAVON_IME_WIX_VERSION} from NuGet"
    VERBATIM
)
add_custom_target(llavon-ime-wix
    DEPENDS
        "${LLAVON_IME_WIX_EXE}"
        "${LLAVON_IME_WIX_MARKER}"
        "${LLAVON_IME_WIX_BAL_MARKER}"
)

add_custom_target(llavon-ime-package-inputs
    DEPENDS
        llavon-ime-artifacts
        llavon-ime-vcpkg-licenses
        llavon-ime-wix
)

add_custom_target(llavon-ime-msi
    COMMAND
        "${CMAKE_COMMAND}" -E env
            "WIX_EXTENSIONS=${LLAVON_IME_WIX_EXTENSIONS_DIR}"
            "${CMAKE_CPACK_COMMAND}"
            -C Release
            --config "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS llavon-ime-package-inputs
    USES_TERMINAL
)

set(LLAVON_IME_MSI_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/llavon-ime-${LLAVON_IME_VERSION}-windows.msi")
set(LLAVON_IME_SETUP_PATH
    "${CMAKE_CURRENT_BINARY_DIR}/llavon-ime-${LLAVON_IME_VERSION}-setup.exe")
set(LLAVON_IME_BUNDLE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/Bundle.wxs")
add_custom_target(llavon-ime-bundle-manifest
    COMMAND "${CMAKE_COMMAND}"
        "-DLLAVON_IME_MODEL_REPO=${LLAVON_IME_MODEL_REPO}"
        "-DLLAVON_IME_MODEL_FILENAME=${LLAVON_IME_MODEL_FILENAME}"
        "-DLLAVON_IME_MODEL_REVISION=${LLAVON_IME_MODEL_REVISION}"
        "-DLLAVON_IME_MODEL_SHA256=${LLAVON_IME_MODEL_SHA256}"
        "-DLLAVON_IME_MODEL_SIZE=${LLAVON_IME_MODEL_SIZE}"
        "-DLLAVON_IME_MODEL_INSTALLER_PATH=$<TARGET_FILE:llavon-ime-model-installer>"
        "-DLLAVON_IME_MSI_PATH=${LLAVON_IME_MSI_PATH}"
        "-DLLAVON_IME_VERSION=${LLAVON_IME_VERSION}"
        "-DLLAVON_IME_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DLLAVON_IME_BUNDLE_TEMPLATE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/Bundle.wxs.in"
        "-DLLAVON_IME_BUNDLE_OUTPUT=${LLAVON_IME_BUNDLE_OUTPUT}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ConfigureBundle.cmake"
    DEPENDS llavon-ime-model-installer
    VERBATIM
)
add_custom_target(llavon-ime-setup
    COMMAND "${CMAKE_COMMAND}" -E env
        "WIX_EXTENSIONS=${LLAVON_IME_WIX_EXTENSIONS_DIR}"
        "${LLAVON_IME_WIX_EXE}" build
        -arch x64
        -ext "WixToolset.BootstrapperApplications.wixext/${LLAVON_IME_WIX_BAL_EXTENSION_VERSION}"
        -o "${LLAVON_IME_SETUP_PATH}"
        "${LLAVON_IME_BUNDLE_OUTPUT}"
    DEPENDS
        llavon-ime-msi
        llavon-ime-bundle-manifest
        "${LLAVON_IME_WIX_EXE}"
        "${LLAVON_IME_WIX_BAL_MARKER}"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/BundleTheme.xml"
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/BundleTheme.zh-TW.wxl"
    USES_TERMINAL
)

# The custom triplet builds llama.cpp and ggml as DLLs, including CPU backend
# modules loaded dynamically at runtime. Install the complete runtime DLL set.
install(DIRECTORY
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/"
    DESTINATION bin
    COMPONENT Runtime
    FILES_MATCHING PATTERN "*.dll"
)
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ime-core/table/"
    DESTINATION tables
    COMPONENT Runtime
)
install(PROGRAMS "${LLAVON_IME_SERVICE_STARTUP_LAUNCHER}"
    DESTINATION bin
    COMPONENT Runtime
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/LICENSE.txt"
    "${CMAKE_CURRENT_SOURCE_DIR}/MODEL-LICENSE.txt"
    DESTINATION licenses
    COMPONENT Runtime
)
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ime-core/LICENSE"
    DESTINATION licenses
    RENAME ime-core-LICENSE.txt
    COMPONENT Runtime
)
install(FILES "${LLAVON_IME_VCPKG_LICENSE_AGGREGATE}"
    DESTINATION licenses
    COMPONENT Runtime
)
install(DIRECTORY "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}/"
    DESTINATION licenses/vcpkg
    COMPONENT Runtime
)

set(CPACK_GENERATOR "WIX")
set(CPACK_COMPONENTS_ALL Runtime)
set(CPACK_COMPONENT_RUNTIME_REQUIRED TRUE)
set(CPACK_PACKAGE_NAME "拉風輸入法")
set(CPACK_PACKAGE_VENDOR "Llavon IME")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "拉風輸入法 Windows 版")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Llavon IME")
set(CPACK_PACKAGING_INSTALL_PREFIX "")
set(CPACK_RESOURCE_FILE_LICENSE "${LLAVON_IME_PACKAGE_LICENSE}")
set(CPACK_PACKAGE_FILE_NAME "llavon-ime-${LLAVON_IME_VERSION}-windows")
# CPack uses "4" for the WiX v4+ CLI and authoring format, including WiX 7.
set(CPACK_WIX_VERSION "4")
set(CPACK_WIX_EXTENSIONS "WixToolset.UI.wixext")
set(CPACK_WIX_CULTURES "zh-TW")
set(CPACK_WIX_TEMPLATE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/WIX.zh-TW.template.in")
set(CPACK_WIX_ROOT "${LLAVON_IME_WIX_TOOLS_DIR}")
set(CPACK_WIX_EXECUTABLE "${LLAVON_IME_WIX_EXE}")
set(CPACK_WIX_SIZEOF_VOID_P "8")
set(CPACK_WIX_ARCHITECTURE "x64")
set(CPACK_WIX_INSTALL_SCOPE "perMachine")
set(CPACK_WIX_UPGRADE_GUID "FD29FA2D-C02C-47CB-AA80-B334BACAABF0")
set(CPACK_WIX_UI_REF "WixUI_InstallDir")
set(CPACK_WIX_PROGRAM_MENU_FOLDER "Llavon IME")
file(TO_CMAKE_PATH
    "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/llavon-ime-lifecycle.exe"
    LLAVON_IME_LIFECYCLE_HELPER_WIX_PATH
)
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/RegisterIme.wxs.in"
    "${CMAKE_CURRENT_BINARY_DIR}/RegisterIme.wxs"
    @ONLY
)
set(CPACK_WIX_PATCH_FILE "${CMAKE_CURRENT_BINARY_DIR}/RegisterIme.wxs")
set(CPACK_WIX_EXTRA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/Autostart.wxs"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/wix/UninstallShortcut.wxs"
)
set(CPACK_WIX_PROPERTY_ARPCOMMENTS "拉風輸入法 Windows 安裝程式")
set(CPACK_WIX_PROPERTY_ARPURLINFOABOUT "https://github.com/llavon-ime")

include(CPack)

if(TARGET package)
    add_dependencies(package llavon-ime-package-inputs)
endif()

if(TARGET PACKAGE)
    add_dependencies(PACKAGE llavon-ime-package-inputs)
endif()
