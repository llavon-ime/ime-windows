file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "src/*.h" "src/*.hpp")
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE RESOURCES CONFIGURE_DEPENDS "src/*.rc")
set(DEF_FILE "${CMAKE_CURRENT_SOURCE_DIR}/src/ime.def")

include_directories(src)

add_library(llavon-ime SHARED ${SOURCES} ${HEADERS} ${RESOURCES} ${DEF_FILE})
add_executable(llavon-ime-lifecycle WIN32
    tools/tsf_lifecycle.cpp
    src/system/serviceLauncher.cpp
)
target_compile_features(llavon-ime PRIVATE cxx_std_23)
target_compile_features(llavon-ime-lifecycle PRIVATE cxx_std_23)
set_target_properties(llavon-ime PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
set_target_properties(llavon-ime-lifecycle PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

target_compile_definitions(llavon-ime PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00        # Windows 10
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(llavon-ime-lifecycle PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)

target_compile_options(llavon-ime PRIVATE
    /W4         # 高警告等級
    /WX         # 警告視為錯誤
    /utf-8      # 原始碼 UTF-8 編碼
)
target_compile_options(llavon-ime-lifecycle PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)

find_package(cppwinrt CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)

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
    utf8::cpp
    utf8cpp::utf8cpp
)
target_link_libraries(llavon-ime-lifecycle PRIVATE
    advapi32
    msi
    ole32
    shell32
    uuid
)


set_target_properties(llavon-ime PROPERTIES
    OUTPUT_NAME "llavon-ime"
    PREFIX      ""
)

install(TARGETS llavon-ime
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)

if(BUILD_TESTING)
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
    add_dependencies(frontend-dll-load-tests llavon-ime)
    add_test(NAME frontend-dll-load
        COMMAND frontend-dll-load-tests "$<TARGET_FILE:llavon-ime>")
endif()
