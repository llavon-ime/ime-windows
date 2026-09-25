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
