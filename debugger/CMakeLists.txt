cmake_minimum_required(VERSION 3.24)

project(llavon_ime_windows_debugger VERSION 0.1.0 LANGUAGES CXX RC)

if(NOT WIN32)
    message(FATAL_ERROR "ime-windows-debugger is Windows-only")
endif()

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)
include(CTest)

find_package(cppwinrt CONFIG REQUIRED)

add_library(llavon-debug-client STATIC
    include/llavon-debug/logger.hpp
    src/bounded_mpmc_queue.hpp
    src/logger.cpp
    src/pipe_protocol.hpp
)
add_library(llavon::debug-client ALIAS llavon-debug-client)

target_compile_features(llavon-debug-client PUBLIC cxx_std_23)
set_target_properties(llavon-debug-client PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    EXPORT_NAME debug-client
)
target_compile_definitions(llavon-debug-client PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_options(llavon-debug-client PRIVATE /W4 /WX /utf-8 /permissive-)
target_include_directories(llavon-debug-client
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

add_executable(llavon-ime-debugger WIN32
    src/debugger/debugger.manifest
    src/debugger/debugger.rc
    src/debugger/debugger_window.cpp
    src/debugger/debugger_window.hpp
    src/debugger/main.cpp
    src/debugger/pipe_server.cpp
    src/debugger/pipe_server.hpp
    src/pipe_protocol.hpp
)
target_compile_features(llavon-ime-debugger PRIVATE cxx_std_23)
set_target_properties(llavon-ime-debugger PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    OUTPUT_NAME "llavon-ime-debugger"
)
target_compile_definitions(llavon-ime-debugger PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_options(llavon-ime-debugger PRIVATE /W4 /WX /utf-8 /permissive-)
target_link_options(llavon-ime-debugger PRIVATE /MANIFEST:NO)
target_include_directories(llavon-ime-debugger PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(llavon-ime-debugger PRIVATE
    Microsoft::CppWinRT
    WindowsApp
    advapi32
    comctl32
    ole32
    runtimeobject
)

if(BUILD_TESTING)
    add_executable(logger-queue-tests
        tests/logger_queue_tests.cpp
        src/bounded_mpmc_queue.hpp
    )
    target_compile_features(logger-queue-tests PRIVATE cxx_std_23)
    target_compile_options(logger-queue-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(logger-queue-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME logger-queue COMMAND logger-queue-tests)
endif()

install(TARGETS llavon-debug-client
    EXPORT llavon-debug-client-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(TARGETS llavon-ime-debugger RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(EXPORT llavon-debug-client-targets
    FILE llavon-debug-client-targets.cmake
    NAMESPACE llavon::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/llavon-debug-client"
)
configure_package_config_file(
    cmake/llavon-debug-client-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/llavon-debug-client-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/llavon-debug-client"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/llavon-debug-client-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/llavon-debug-client-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/llavon-debug-client-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/llavon-debug-client"
)
