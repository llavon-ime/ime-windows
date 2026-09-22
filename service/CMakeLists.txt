if(NOT WIN32)
    message(FATAL_ERROR "The Llavon IME service is Windows-only")
endif()

find_package(asio CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)

add_executable(llavon-ime-service
    src/assets/llavon_tray.jpg
    src/assets/llavon_tray.ico
    src/main.cpp
    src/resource.h
    src/service/candidate_pipe_server.hpp
    src/service/candidate_pipe_protocol.hpp
    src/service/candidate_ui_loader.cpp
    src/service/candidate_ui_loader.hpp
    src/service/custom_name_matcher.cpp
    src/service/custom_name_matcher.hpp
    src/service/debug/core_logger_adapter.hpp
    src/service/user_settings.cpp
    src/service/user_settings.hpp
    src/service/user_settings_codec.cpp
    src/service/user_settings_codec.hpp
    src/service/prediction_pipe_server.hpp
    src/service/settings_ui_loader.cpp
    src/service/settings_ui_loader.hpp
    src/service/tray_icon.cpp
    src/service/tray_icon.hpp
    src/service/training_data_writer.cpp
    src/service/training_data_writer.hpp
    src/service.rc
)

add_library(llavon-ime-candidate-ui SHARED
    src/candidate/candidate_log.hpp
    src/candidate/candidate_runtime.cpp
    src/candidate/candidate_ui_api.h
    src/candidate/candidate_window.hpp
    src/candidate/window.hpp
)

add_library(llavon-ime-settings-ui SHARED
    src/settings/settings_configuration.hpp
    src/settings/settings_menu_window.cpp
    src/settings/settings_menu_window.hpp
    src/settings/settings_runtime.cpp
    src/settings/settings_ui_api.h
    src/settings/settings_window.cpp
    src/settings/settings_window.hpp
    src/settings/update_checker.cpp
    src/settings/update_checker.hpp
)

target_compile_features(llavon-ime-service PRIVATE cxx_std_23)
target_compile_features(llavon-ime-candidate-ui PRIVATE cxx_std_23)
target_compile_features(llavon-ime-settings-ui PRIVATE cxx_std_23)
set_target_properties(llavon-ime-service PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
set_target_properties(llavon-ime-settings-ui PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    OUTPUT_NAME "llavon-ime-settings-ui"
)
set_target_properties(llavon-ime-candidate-ui PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    OUTPUT_NAME "llavon-ime-candidate-ui"
)
target_compile_definitions(llavon-ime-service PRIVATE
    LLAVON_IME_VERSION="${PROJECT_VERSION}"
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(llavon-ime-settings-ui PRIVATE
    LLAVON_IME_BUILD_COMMIT="${IME_WINDOWS_SERVICE_BUILD_COMMIT}"
    LLAVON_IME_BUILD_NUMBER=${IME_WINDOWS_SERVICE_BUILD_NUMBER}
    LLAVON_SETTINGS_UI_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_definitions(llavon-ime-candidate-ui PRIVATE
    LLAVON_CANDIDATE_UI_EXPORTS
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
)
target_compile_options(llavon-ime-service PRIVATE
    /W4
    /WX
    /utf-8
    /await:strict
)
target_compile_options(llavon-ime-settings-ui PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)
target_compile_options(llavon-ime-candidate-ui PRIVATE
    /W4
    /WX
    /utf-8
    /permissive-
)
target_link_options(llavon-ime-service PRIVATE /MANIFEST:NO)
target_include_directories(llavon-ime-service PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(llavon-ime-settings-ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/settings"
)
target_include_directories(llavon-ime-candidate-ui PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src/candidate"
)
target_link_libraries(llavon-ime-service PRIVATE
    ime-core::ime-core
    llavon::debug-client
    asio::asio
    reflectcpp::reflectcpp
    utf8cpp::utf8cpp
    advapi32
    ole32
    oleaut32
    shell32
    ws2_32
)
target_link_libraries(llavon-ime-settings-ui PRIVATE
    Microsoft::CppWinRT
    WindowsApp
    reflectcpp::reflectcpp
    utf8cpp::utf8cpp
    comctl32
    dwmapi
    ole32
    runtimeobject
)
target_link_libraries(llavon-ime-candidate-ui PRIVATE
    Microsoft::CppWinRT
    WindowsApp
    dwmapi
    ole32
    runtimeobject
)

if(BUILD_TESTING)
    add_executable(candidate-pipe-protocol-tests
        tests/candidate_pipe_protocol_tests.cpp
        src/service/candidate_pipe_protocol.hpp
    )
    target_compile_features(candidate-pipe-protocol-tests PRIVATE cxx_std_23)
    target_compile_definitions(candidate-pipe-protocol-tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
    target_compile_options(candidate-pipe-protocol-tests PRIVATE
        /W4
        /WX
        /utf-8
        /permissive-
    )
    target_include_directories(candidate-pipe-protocol-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    add_test(NAME candidate-pipe-protocol COMMAND candidate-pipe-protocol-tests)

    add_executable(training-data-writer-tests
        tests/training_data_writer_tests.cpp
        src/service/training_data_writer.cpp
        src/service/training_data_writer.hpp
    )
    target_compile_features(training-data-writer-tests PRIVATE cxx_std_23)
    target_compile_definitions(training-data-writer-tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    target_compile_options(training-data-writer-tests PRIVATE
        /W4
        /WX
        /utf-8
        /permissive-
    )
    target_include_directories(training-data-writer-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
    )
    target_link_libraries(training-data-writer-tests PRIVATE
        utf8::cpp
        utf8cpp::utf8cpp
        ole32
        shell32
    )
    add_test(NAME training-data-writer COMMAND training-data-writer-tests)

endif()

install(TARGETS
    llavon-ime-service
    llavon-ime-candidate-ui
    llavon-ime-settings-ui
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)
