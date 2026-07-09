foreach(_required_var IN ITEMS
    LLAVON_IME_WIX_VERSION
    LLAVON_IME_WIX_UI_EXTENSION_VERSION
    LLAVON_IME_WIX_TOOLS_DIR
    LLAVON_IME_WIX_EXTENSIONS_DIR
    LLAVON_IME_WIX_EXE
    LLAVON_IME_WIX_MARKER
)
    if(NOT DEFINED ${_required_var} OR "${${_required_var}}" STREQUAL "")
        message(FATAL_ERROR "${_required_var} is required")
    endif()
endforeach()

find_program(DOTNET_EXECUTABLE dotnet REQUIRED)

file(MAKE_DIRECTORY "${LLAVON_IME_WIX_TOOLS_DIR}")
file(MAKE_DIRECTORY "${LLAVON_IME_WIX_EXTENSIONS_DIR}")

set(_wix_package_dir "${LLAVON_IME_WIX_TOOLS_DIR}/wix-${LLAVON_IME_WIX_VERSION}")
set(_wix_package_path "${LLAVON_IME_WIX_TOOLS_DIR}/wix.${LLAVON_IME_WIX_VERSION}.nupkg")
set(_wix_package_stamp "${_wix_package_dir}/.extracted")
set(_wix_package_url "https://www.nuget.org/api/v2/package/wix/${LLAVON_IME_WIX_VERSION}")

if(NOT EXISTS "${_wix_package_stamp}")
    file(REMOVE_RECURSE "${_wix_package_dir}")
    file(MAKE_DIRECTORY "${_wix_package_dir}")

    if(NOT EXISTS "${_wix_package_path}")
        message(STATUS "Downloading WiX ${LLAVON_IME_WIX_VERSION} from NuGet")
        file(DOWNLOAD
            "${_wix_package_url}"
            "${_wix_package_path}"
            SHOW_PROGRESS
            STATUS _wix_download_status
            TLS_VERIFY ON
        )
        list(GET _wix_download_status 0 _wix_download_code)
        list(GET _wix_download_status 1 _wix_download_message)
        if(NOT _wix_download_code EQUAL 0)
            file(REMOVE "${_wix_package_path}")
            message(FATAL_ERROR "WiX NuGet download failed: ${_wix_download_message}")
        endif()
    endif()

    file(ARCHIVE_EXTRACT
        INPUT "${_wix_package_path}"
        DESTINATION "${_wix_package_dir}"
    )

    file(WRITE "${_wix_package_stamp}" "")
endif()

file(GLOB_RECURSE _wix_dll_candidates
    LIST_DIRECTORIES FALSE
    "${_wix_package_dir}/tools/*/any/wix.dll"
    "${_wix_package_dir}/tools/*/wix.dll"
)

if(NOT _wix_dll_candidates)
    message(FATAL_ERROR "Unable to find wix.dll in ${_wix_package_path}")
endif()

list(GET _wix_dll_candidates 0 _wix_dll)

file(TO_NATIVE_PATH "${DOTNET_EXECUTABLE}" _dotnet_native)
file(TO_NATIVE_PATH "${_wix_dll}" _wix_dll_native)
file(WRITE "${LLAVON_IME_WIX_EXE}" "@echo off\r\n\"${_dotnet_native}\" \"${_wix_dll_native}\" %*\r\n")

execute_process(
    COMMAND "${LLAVON_IME_WIX_EXE}" --version
    OUTPUT_VARIABLE _wix_version_output
    ERROR_VARIABLE _wix_version_error
    RESULT_VARIABLE _wix_version_result
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

if(NOT _wix_version_result EQUAL 0)
    message(FATAL_ERROR "Failed to run WiX: ${_wix_version_error}")
endif()

if(NOT _wix_version_output VERSION_EQUAL "${LLAVON_IME_WIX_VERSION}")
    message(FATAL_ERROR "Expected WiX ${LLAVON_IME_WIX_VERSION}, got ${_wix_version_output}")
endif()

if(NOT EXISTS "${LLAVON_IME_WIX_MARKER}")
    execute_process(
        COMMAND
            "${CMAKE_COMMAND}" -E env
                "WIX_EXTENSIONS=${LLAVON_IME_WIX_EXTENSIONS_DIR}"
                "${LLAVON_IME_WIX_EXE}" extension add --global
                "WixToolset.UI.wixext/${LLAVON_IME_WIX_UI_EXTENSION_VERSION}"
        RESULT_VARIABLE _wix_extension_result
    )

    if(NOT _wix_extension_result EQUAL 0)
        message(FATAL_ERROR "Failed to install WixToolset.UI.wixext/${LLAVON_IME_WIX_UI_EXTENSION_VERSION}")
    endif()

    file(WRITE "${LLAVON_IME_WIX_MARKER}" "")
endif()
