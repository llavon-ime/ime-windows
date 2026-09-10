if(NOT DEFINED LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR)
    message(FATAL_ERROR "LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR is required")
endif()
if(NOT DEFINED LLAVON_IME_VCPKG_LICENSE_AGGREGATE)
    message(FATAL_ERROR "LLAVON_IME_VCPKG_LICENSE_AGGREGATE is required")
endif()
if(NOT DEFINED LLAVON_IME_VCPKG_LICENSE_MARKER)
    message(FATAL_ERROR "LLAVON_IME_VCPKG_LICENSE_MARKER is required")
endif()
if(NOT DEFINED VCPKG_INSTALLED_DIR)
    message(FATAL_ERROR "VCPKG_INSTALLED_DIR is required")
endif()
if(NOT DEFINED VCPKG_TARGET_TRIPLET)
    message(FATAL_ERROR "VCPKG_TARGET_TRIPLET is required")
endif()

set(_share_dir
    "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/share")
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

file(REMOVE_RECURSE "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}")
file(MAKE_DIRECTORY "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}")
set(_aggregate "# Third-party vcpkg licenses\n\n")
string(APPEND _aggregate
    "This file is generated from the unified dependency manifest for the "
    "Llavon IME Windows distribution.\n\n"
)

foreach(_copyright_file IN LISTS _copyright_files)
    get_filename_component(_package_dir "${_copyright_file}" DIRECTORY)
    get_filename_component(_package_name "${_package_dir}" NAME)
    file(COPY_FILE
        "${_copyright_file}"
        "${LLAVON_IME_VCPKG_LICENSE_OUTPUT_DIR}/${_package_name}.txt"
        ONLY_IF_DIFFERENT
    )
    file(READ "${_copyright_file}" _copyright_text)
    string(APPEND _aggregate
        "==== ${_package_name} ====\n\n"
        "${_copyright_text}\n\n"
    )
endforeach()

file(WRITE "${LLAVON_IME_VCPKG_LICENSE_AGGREGATE}" "${_aggregate}")
file(WRITE "${LLAVON_IME_VCPKG_LICENSE_MARKER}" "ok\n")
