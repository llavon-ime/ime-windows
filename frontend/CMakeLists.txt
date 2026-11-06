include_directories(src)

add_executable(llavon-ime-lifecycle WIN32
    tools/tsf_lifecycle.cpp
    src/system/serviceLauncher.cpp
)
add_executable(llavon-ime-uninstaller WIN32
    tools/uninstaller.cpp
    tools/uninstall_purge.cpp
    tools/uninstall_purge.hpp
)
target_compile_features(llavon-ime-lifecycle PRIVATE cxx_std_23)
target_compile_features(llavon-ime-uninstaller PRIVATE cxx_std_23)
set_target_properties(llavon-ime-lifecycle llavon-ime-uninstaller PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(llavon-ime-lifecycle PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(llavon-ime-uninstaller PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_compile_options(llavon-ime-lifecycle PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)
target_compile_options(llavon-ime-uninstaller PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)

find_package(cppwinrt CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)

target_link_libraries(llavon-ime-lifecycle PRIVATE
    llavon::ipc
    advapi32
    msi
    ole32
    shell32
    uuid
)
target_link_libraries(llavon-ime-uninstaller PRIVATE
    advapi32
    ole32
    shell32
)

if(LLAVON_IME_FRONTEND_ONLY)
    file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "src/*.h" "src/*.hpp")
    file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
    file(GLOB_RECURSE RESOURCES CONFIGURE_DEPENDS "src/*.rc")
    set(DEF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/ime.def")

    add_library(llavon-ime SHARED ${SOURCES} ${HEADERS} ${RESOURCES} ${DEF_FILE})
    target_compile_features(llavon-ime PRIVATE cxx_std_23)
    set_target_properties(llavon-ime PROPERTIES
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        OUTPUT_NAME "llavon-ime"
        PREFIX      ""
    )
    target_compile_definitions(llavon-ime PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        WINVER=0x0A00        # Windows 10
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(llavon-ime PRIVATE
        /W4         # 高警告等級
        /WX         # 警告視為錯誤
        /utf-8      # 原始碼 UTF-8 編碼
    )

    # A shared yyjson dependency prevents AppContainer hosts from loading the TIP.
    # Check custom triplets too, before producing an unusable installer.
    get_target_property(_llavon_yyjson_type yyjson::yyjson TYPE)
    if(NOT _llavon_yyjson_type STREQUAL "STATIC_LIBRARY")
        message(FATAL_ERROR
            "The TSF frontend requires static yyjson. Configure with the repository's "
            "cmake/triplets overlay and rebuild the vcpkg dependencies.")
    endif()

    target_link_libraries(llavon-ime PRIVATE
        Microsoft::CppWinRT
        WindowsApp
        advapi32    # Registry API
        ole32       # COM
        oleaut32    # COM Automation
        uuid        # TSF GUID constants
        reflectcpp::reflectcpp
        llavon::debug-client
        llavon::ipc
        utf8::cpp
        utf8cpp::utf8cpp
    )

    if(CMAKE_SIZEOF_VOID_P EQUAL 4)
        set(_llavon_frontend_install_dir bin/x86)
    else()
        set(_llavon_frontend_install_dir bin)
    endif()
    install(TARGETS llavon-ime
        RUNTIME DESTINATION "${_llavon_frontend_install_dir}"
        COMPONENT Runtime
    )
endif()

if(BUILD_TESTING)
    add_executable(frontend-mapping-layout-tests tests/mapping_layout_tests.cpp)
    target_compile_features(frontend-mapping-layout-tests PRIVATE cxx_std_23)
    target_compile_definitions(frontend-mapping-layout-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(frontend-mapping-layout-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(frontend-mapping-layout-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(frontend-mapping-layout-tests PRIVATE
        reflectcpp::reflectcpp utf8::cpp utf8cpp::utf8cpp)
    foreach(_layout IN ITEMS installed-x64 installed-x86 development-x64 development-x86)
        add_test(NAME frontend-mapping-${_layout}
            COMMAND "${CMAKE_COMMAND}"
                "-DTEST_EXECUTABLE=$<TARGET_FILE:frontend-mapping-layout-tests>"
                "-DMAPPING_FILE=${CMAKE_SOURCE_DIR}/ime-core/table/bopomofo_char.json"
                "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/mapping-tests/$<CONFIG>/${_layout}"
                "-DLAYOUT=${_layout}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/run_mapping_layout_test.cmake")
        set_tests_properties(frontend-mapping-${_layout} PROPERTIES TIMEOUT 30)
    endforeach()

    add_executable(frontend-engine-worker-tests
        tests/engine_worker_tests.cpp
        src/core/composition_controller.cpp
        src/core/sealed_text.cpp
        src/engine/engine_worker.cpp)
    target_include_directories(frontend-engine-worker-tests PRIVATE src)
    target_compile_features(frontend-engine-worker-tests PRIVATE cxx_std_23)
    target_compile_definitions(frontend-engine-worker-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(frontend-engine-worker-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(frontend-engine-worker-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(frontend-engine-worker-tests PRIVATE
        ole32 Microsoft::CppWinRT WindowsApp reflectcpp::reflectcpp utf8::cpp utf8cpp::utf8cpp)
    add_test(NAME frontend-engine-worker COMMAND frontend-engine-worker-tests)
    set_tests_properties(frontend-engine-worker PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "LLAVON_IME_TABLES_DIR=${CMAKE_SOURCE_DIR}/ime-core/table")

    add_executable(frontend-typing-timing-tests
        tests/typing_timing_tests.cpp
        src/core/composition_controller.cpp
        src/core/sealed_text.cpp
        src/engine/engine_worker.cpp)
    target_compile_features(frontend-typing-timing-tests PRIVATE cxx_std_23)
    target_compile_definitions(frontend-typing-timing-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(frontend-typing-timing-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(frontend-typing-timing-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(frontend-typing-timing-tests PRIVATE
        ole32 Microsoft::CppWinRT WindowsApp reflectcpp::reflectcpp utf8::cpp utf8cpp::utf8cpp)
    add_test(NAME frontend-typing-timing COMMAND frontend-typing-timing-tests)
    set_tests_properties(frontend-typing-timing PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "LLAVON_IME_TABLES_DIR=${CMAKE_SOURCE_DIR}/ime-core/table")

    add_executable(frontend-key-router-tests
        tests/key_router_tests.cpp
        src/tsf/key_router.cpp)
    target_include_directories(frontend-key-router-tests PRIVATE src)
    target_compile_features(frontend-key-router-tests PRIVATE cxx_std_23)
    target_compile_definitions(frontend-key-router-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(frontend-key-router-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(frontend-key-router-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(frontend-key-router-tests PRIVATE
        reflectcpp::reflectcpp utf8::cpp utf8cpp::utf8cpp)
    add_test(NAME frontend-key-router COMMAND frontend-key-router-tests)
    set_tests_properties(frontend-key-router PROPERTIES
        TIMEOUT 30
        ENVIRONMENT "LLAVON_IME_TABLES_DIR=${CMAKE_SOURCE_DIR}/ime-core/table")

    add_executable(frontend-dll-load-tests tests/dll_load_tests.cpp)
    target_compile_features(frontend-dll-load-tests PRIVATE cxx_std_23)
    target_compile_definitions(frontend-dll-load-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(frontend-dll-load-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(frontend-dll-load-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES
        CXX_EXTENSIONS NO
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    if(TARGET llavon-ime)
        add_dependencies(frontend-dll-load-tests llavon-ime)
        set(_llavon_frontend_dll "$<TARGET_FILE:llavon-ime>")
    else()
        add_dependencies(frontend-dll-load-tests llavon-ime-frontend-x64)
        set(_llavon_frontend_dll "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/llavon-ime.dll")
    endif()
    target_link_libraries(frontend-dll-load-tests PRIVATE uuid)
    add_test(NAME frontend-dll-load
        COMMAND frontend-dll-load-tests "${_llavon_frontend_dll}")

    add_executable(uninstall-purge-tests
        tests/uninstall_purge_tests.cpp
        tools/uninstall_purge.cpp
        tools/uninstall_purge.hpp
    )
    target_compile_features(uninstall-purge-tests PRIVATE cxx_std_23)
    target_compile_definitions(uninstall-purge-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(uninstall-purge-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(uninstall-purge-tests PRIVATE tools)
    set_target_properties(uninstall-purge-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_link_libraries(uninstall-purge-tests PRIVATE advapi32)
    add_dependencies(uninstall-purge-tests llavon-ime-uninstaller)
    add_test(NAME uninstall-purge
        COMMAND uninstall-purge-tests "$<TARGET_FILE:llavon-ime-uninstaller>")
    set_tests_properties(uninstall-purge PROPERTIES TIMEOUT 60)
endif()
