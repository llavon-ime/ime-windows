add_library(llavon-ipc INTERFACE)
add_library(llavon::ipc ALIAS llavon-ipc)

target_compile_features(llavon-ipc INTERFACE cxx_std_23)
target_include_directories(llavon-ipc INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llavon-ipc INTERFACE advapi32)

if(BUILD_TESTING)
    add_executable(pipe-security-tests tests/pipe_security_tests.cpp)
    target_link_libraries(pipe-security-tests PRIVATE llavon::ipc authz)
    target_compile_definitions(pipe-security-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(pipe-security-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(pipe-security-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME pipe-security COMMAND pipe-security-tests)
    set_tests_properties(pipe-security PROPERTIES TIMEOUT 30)

    add_executable(pipe-appcontainer-tests
        tests/pipe_appcontainer_tests.cpp
        "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/src/system/serviceLauncher.cpp")
    target_include_directories(pipe-appcontainer-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/../frontend/src/system")
    target_link_libraries(pipe-appcontainer-tests PRIVATE llavon::ipc user32)
    target_compile_definitions(pipe-appcontainer-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(pipe-appcontainer-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(pipe-appcontainer-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    add_test(NAME pipe-appcontainer COMMAND pipe-appcontainer-tests)
    set_tests_properties(pipe-appcontainer PROPERTIES TIMEOUT 60)
endif()
