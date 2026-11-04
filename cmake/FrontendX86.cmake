include(ExternalProject)

if(NOT CMAKE_GENERATOR MATCHES "^Visual Studio")
    message(FATAL_ERROR "The dual-architecture build requires a Visual Studio generator")
endif()

set(LLAVON_IME_X86_BINARY_DIR "${CMAKE_BINARY_DIR}/frontend-x86")
ExternalProject_Add(llavon-ime-frontend-x86
    SOURCE_DIR "${CMAKE_SOURCE_DIR}"
    BINARY_DIR "${LLAVON_IME_X86_BINARY_DIR}"
    CMAKE_GENERATOR "${CMAKE_GENERATOR}"
    CMAKE_GENERATOR_PLATFORM Win32
    CMAKE_GENERATOR_INSTANCE "${CMAKE_GENERATOR_INSTANCE}"
    CMAKE_GENERATOR_TOOLSET "${CMAKE_GENERATOR_TOOLSET}"
    CMAKE_ARGS
        "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}"
        "-DVCPKG_OVERLAY_TRIPLETS=${CMAKE_SOURCE_DIR}/cmake/triplets"
        "-DVCPKG_TARGET_TRIPLET=x86-windows-static"
        "-DVCPKG_INSTALL_OPTIONS=${VCPKG_INSTALL_OPTIONS}"
        "-DLLAVON_IME_FRONTEND_ONLY=ON"
        "-DBUILD_TESTING=${BUILD_TESTING}"
    BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR>
        --config $<CONFIG> --target llavon-ime
    BUILD_ALWAYS TRUE
    BUILD_BYPRODUCTS "${LLAVON_IME_X86_BINARY_DIR}/bin/llavon-ime.dll"
    INSTALL_COMMAND ""
)

install(FILES "${LLAVON_IME_X86_BINARY_DIR}/bin/llavon-ime.dll"
    DESTINATION bin/x86 COMPONENT Runtime)

if(BUILD_TESTING)
    add_test(NAME pipe-cross-architecture-appcontainer
        COMMAND pipe-appcontainer-tests --client-executable
            "${LLAVON_IME_X86_BINARY_DIR}/bin/pipe-appcontainer-tests.exe")
    set_tests_properties(pipe-cross-architecture-appcontainer PROPERTIES TIMEOUT 60)
endif()
