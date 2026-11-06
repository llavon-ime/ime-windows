include(ExternalProject)

if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
    message(FATAL_ERROR "The dual-architecture build requires a Visual Studio generator")
endif()

function(llavon_add_static_frontend architecture generator_platform)
    set(binary_dir "${CMAKE_BINARY_DIR}/frontend-${architecture}")
    ExternalProject_Add(llavon-ime-frontend-${architecture}
        SOURCE_DIR "${CMAKE_SOURCE_DIR}"
        BINARY_DIR "${binary_dir}"
        CMAKE_GENERATOR "${CMAKE_GENERATOR}"
        CMAKE_GENERATOR_PLATFORM "${generator_platform}"
        CMAKE_GENERATOR_INSTANCE "${CMAKE_GENERATOR_INSTANCE}"
        CMAKE_GENERATOR_TOOLSET "${CMAKE_GENERATOR_TOOLSET}"
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
            "-DVCPKG_OVERLAY_TRIPLETS=${CMAKE_SOURCE_DIR}/cmake/triplets"
            "-DVCPKG_TARGET_TRIPLET=${architecture}-windows-static"
            "-DVCPKG_INSTALL_OPTIONS=${VCPKG_INSTALL_OPTIONS}"
            "-DLLAVON_IME_FRONTEND_ONLY=ON"
            "-DBUILD_TESTING=${BUILD_TESTING}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
            --config $<CONFIG> --target llavon-ime
        BUILD_ALWAYS TRUE
        BUILD_BYPRODUCTS "${binary_dir}/bin/llavon-ime.dll"
        INSTALL_COMMAND ""
    )
    set(LLAVON_IME_FRONTEND_${architecture}_BINARY_DIR "${binary_dir}" PARENT_SCOPE)
endfunction()

llavon_add_static_frontend(x64 x64)
llavon_add_static_frontend(x86 Win32)

set(LLAVON_IME_FRONTEND_x64_DEV_DLL "${CMAKE_BINARY_DIR}/bin/llavon-ime.dll")
ExternalProject_Add_Step(llavon-ime-frontend-x64 publish-dev-dll
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${LLAVON_IME_FRONTEND_x64_BINARY_DIR}/bin/llavon-ime.dll"
        "${LLAVON_IME_FRONTEND_x64_DEV_DLL}"
    DEPENDEES build
    DEPENDERS install
    ALWAYS TRUE
    BYPRODUCTS "${LLAVON_IME_FRONTEND_x64_DEV_DLL}"
)

install(FILES "${LLAVON_IME_FRONTEND_x64_BINARY_DIR}/bin/llavon-ime.dll"
    DESTINATION bin COMPONENT Runtime)
install(FILES "${LLAVON_IME_FRONTEND_x86_BINARY_DIR}/bin/llavon-ime.dll"
    DESTINATION bin/x86 COMPONENT Runtime)

if(BUILD_TESTING)
    add_test(NAME pipe-cross-architecture-appcontainer
        COMMAND pipe-appcontainer-tests --client-executable
            "${LLAVON_IME_FRONTEND_x86_BINARY_DIR}/bin/pipe-appcontainer-tests.exe")
    set_tests_properties(pipe-cross-architecture-appcontainer PROPERTIES TIMEOUT 60)
endif()
