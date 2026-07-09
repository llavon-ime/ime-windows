if(NOT DEFINED LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR)
    message(FATAL_ERROR "LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR is required")
endif()

if(NOT DEFINED LLAVON_IME_VCPKG_LICENSE_AGGREGATE)
    message(FATAL_ERROR "LLAVON_IME_VCPKG_LICENSE_AGGREGATE is required")
endif()

if(NOT DEFINED LLAVON_IME_VCPKG_LICENSE_MARKER)
    message(FATAL_ERROR "LLAVON_IME_VCPKG_LICENSE_MARKER is required")
endif()

if(NOT DEFINED LLAVON_IME_VCPKG_TRIPLET)
    set(LLAVON_IME_VCPKG_TRIPLET "x64-windows")
endif()

file(REMOVE_RECURSE "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}")

set(_aggregate "# Third-party vcpkg licenses\n\n")
string(APPEND _aggregate
    "This file is generated during packaging from the vcpkg packages installed "
    "for the llavon-ime-service and ime-windows-frontend child projects.\n\n"
)

macro(_llavon_collect_vcpkg_licenses project_name installed_root)
    set(_share_dir "${installed_root}/${LLAVON_IME_VCPKG_TRIPLET}/share")
    if(NOT IS_DIRECTORY "${_share_dir}")
        message(FATAL_ERROR "vcpkg share directory was not found: ${_share_dir}")
    endif()

    file(GLOB _copyright_files
        LIST_DIRECTORIES false
        "${_share_dir}/*/copyright"
    )
    list(SORT _copyright_files)

    if(NOT _copyright_files)
        message(FATAL_ERROR "No vcpkg copyright files found under: ${_share_dir}")
    endif()

    set(_project_output_dir "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}/${project_name}")
    file(MAKE_DIRECTORY "${_project_output_dir}")

    foreach(_copyright_file IN LISTS _copyright_files)
        get_filename_component(_package_dir "${_copyright_file}" DIRECTORY)
        get_filename_component(_package_name "${_package_dir}" NAME)

        set(_output_file "${_project_output_dir}/${_package_name}.txt")
        file(COPY_FILE "${_copyright_file}" "${_output_file}" ONLY_IF_DIFFERENT)

        file(READ "${_copyright_file}" _copyright_text)
        string(APPEND _aggregate
            "==== ${project_name} / ${_package_name} ====\n\n"
            "${_copyright_text}\n\n"
        )
    endforeach()
endmacro()

_llavon_collect_vcpkg_licenses(
    "ime-service"
    "${LLAVON_IME_SERVICE_VCPKG_INSTALLED_DIR}"
)
_llavon_collect_vcpkg_licenses(
    "ime-windows-frontend"
    "${LLAVON_IME_FRONTEND_VCPKG_INSTALLED_DIR}"
)

file(WRITE "${LLAVON_IME_VCPKG_LICENSE_AGGREGATE}" "${_aggregate}")
file(WRITE "${LLAVON_IME_VCPKG_LICENSE_MARKER}" "ok\n")
