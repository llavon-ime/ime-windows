if(NOT WIN32)
    message(FATAL_ERROR "The Llavon IME debugger is Windows-only")
endif()

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
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
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

install(TARGETS llavon-ime-debugger
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)
