if(NOT WIN32)
    message(FATAL_ERROR "The Llavon IME service is Windows-only")
endif()

find_package(unofficial-sodium CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(cppwinrt CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)
find_package(VulkanHeaders CONFIG REQUIRED)

add_executable(llavon-ime-service
    src/assets/llavon_tray.jpg
    src/assets/llavon_tray.ico
    src/main.cpp
    src/resource.h
    src/service/candidate_pipe_server.hpp
    src/service/candidate_pipe_protocol.hpp
    src/service/custom_name_matcher.cpp
    src/service/custom_name_matcher.hpp
    src/service/amd_gpu_latency_boost.cpp
    src/service/amd_gpu_latency_boost.hpp
    src/service/amd_gpu_pci_address.hpp
    src/service/amd_gpu_boost_worker.cpp
    src/service/amd_gpu_boost_worker.hpp
    src/service/amd_vulkan_identity.cpp
    src/service/amd_vulkan_identity.hpp
    src/service/gpu_activity_lease.hpp
    src/service/gpu_latency_boost.cpp
    src/service/gpu_latency_boost.hpp
    src/service/commit_crypto.hpp
    src/service/training_data_cleanup.hpp
    src/service/lora_dataset_builder.cpp
    src/service/lora_dataset_builder.hpp
    src/service/base_model_fingerprint.cpp
    src/service/base_model_fingerprint.hpp
    src/service/lora_training_manager.cpp
    src/service/lora_training_manager.hpp
    src/service/lora_trainer_cleanup.hpp
    src/service/lora_process.cpp
    src/service/lora_process.hpp
    src/service/lora_training_presets.hpp
    src/service/major_update_notifier.cpp
    src/service/major_update_notifier.hpp
    src/service/debug/core_logger_adapter.hpp
    src/service/user_settings.cpp
    src/service/user_settings.hpp
    src/service/user_settings_codec.cpp
    src/service/user_settings_codec.hpp
    src/service/prediction_pipe_server.hpp
    src/service/tray_icon.cpp
    src/service/tray_icon.hpp
    src/service/training_data_writer.cpp
    src/service/training_data_writer.hpp
    src/service/winrt_http.hpp
    src/service/process_mta.hpp
    src/settings/update_checker.hpp
    src/service.rc
    src/service.manifest
    src/ui_resources.rc
)

# Embedded only in the web setup. The MSI never carries the GGUF payload.
add_executable(llavon-ime-model-installer
    src/model_installer_main.cpp
    src/service/model_revision_cleanup.cpp
    src/service/model_revision_cleanup.hpp
    src/service/winrt_http.cpp
    src/service/winrt_http.hpp
    src/service/process_mta.hpp
)
target_compile_features(llavon-ime-model-installer PRIVATE cxx_std_23)
target_compile_definitions(llavon-ime-model-installer PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
    WINVER=0x0A00 _WIN32_WINNT=0x0A00)
target_compile_options(llavon-ime-model-installer PRIVATE /W4 /WX /utf-8 /await:strict)
target_link_libraries(llavon-ime-model-installer PRIVATE
    Microsoft::CppWinRT WindowsApp bcrypt shell32)

# Internal modules remain separate compilation units, linked into the service EXE.
# OBJECT libraries also let desktop integration tests host the same implementation.
add_library(llavon-ime-ui OBJECT
    src/ui/ui_thread.cpp
    src/ui/winui_runtime.cpp
    src/candidate/candidate_runtime.cpp
    src/settings/settings_runtime.cpp
    src/settings/settings_window.cpp
    src/settings/settings_menu_window.cpp
    src/settings/lora_history_tree.cpp
    src/settings/lora_history_lca.cpp
    src/settings/update_installer.cpp
    src/settings/update_checker.cpp
    src/service/winrt_http.cpp
)

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/WinUI3.cmake")
llavon_target_winui3(llavon-ime-ui NO_DEPLOY)
llavon_target_winui3(llavon-ime-service)

foreach(target llavon-ime-ui llavon-ime-service)
    target_compile_features(${target} PRIVATE cxx_std_23)
    set_target_properties(${target} PROPERTIES CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_compile_definitions(${target} PRIVATE
        LLAVON_IME_VERSION="${LLAVON_IME_VERSION}"
        LLAVON_IME_BUILD_COMMIT="${IME_WINDOWS_SERVICE_BUILD_COMMIT}"
        LLAVON_IME_BUILD_NUMBER=${IME_WINDOWS_SERVICE_BUILD_NUMBER}
        LLAVON_LORA_SUBMODULE_COMMIT="${LLAVON_LORA_SUBMODULE_COMMIT}"
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(${target} PRIVATE /W4 /WX /utf-8 /permissive-
        /FI"${CMAKE_CURRENT_SOURCE_DIR}/src/ui/winui_platform.hpp")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endforeach()

target_link_libraries(llavon-ime-ui PUBLIC
    ime-core::ime-core llavon::debug-client Microsoft::CppWinRT WindowsApp
    reflectcpp::reflectcpp utf8cpp::utf8cpp unofficial-sodium::sodium
    comctl32 dwmapi ole32 runtimeobject shell32)

target_link_libraries(llavon-ime-service PRIVATE
    llavon-ime-ui ime-core::ime-core llavon::debug-client llavon::ipc
    Microsoft::CppWinRT WindowsApp asio::asio Vulkan::Headers
    reflectcpp::reflectcpp utf8cpp::utf8cpp unofficial-sodium::sodium
    unofficial::sqlite3::sqlite3 advapi32 bcrypt comctl32 d3d11 dwmapi dxgi
    gdi32 ole32 oleaut32 runtimeobject shell32 ws2_32)
set_target_properties(llavon-ime-service PROPERTIES WIN32_EXECUTABLE YES)
target_link_options(llavon-ime-service PRIVATE /ENTRY:mainCRTStartup)

if(BUILD_TESTING)
    add_executable(prediction-display-settings-tests
        tests/prediction_display_settings_tests.cpp
        src/service/user_settings_codec.cpp)
    target_compile_features(prediction-display-settings-tests PRIVATE cxx_std_23)
    target_compile_options(prediction-display-settings-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    set_target_properties(prediction-display-settings-tests PROPERTIES
        CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
    target_include_directories(prediction-display-settings-tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/ime-core/include")
    target_link_libraries(prediction-display-settings-tests PRIVATE
        reflectcpp::reflectcpp utf8cpp::utf8cpp)
    add_test(NAME prediction-display-settings COMMAND prediction-display-settings-tests)

    option(LLAVON_IME_ENABLE_UI_TESTS "Run WinUI integration tests on an interactive Windows desktop" OFF)
    if(LLAVON_IME_ENABLE_UI_TESTS)
        add_executable(candidate-ui-tests tests/candidate_ui_tests.cpp src/service.rc src/service.manifest)
        # The service deploys the shared framework assets before this test host runs.
        add_dependencies(candidate-ui-tests llavon-ime-service)
        target_compile_features(candidate-ui-tests PRIVATE cxx_std_23)
        target_compile_definitions(candidate-ui-tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        target_compile_options(candidate-ui-tests PRIVATE /W4 /WX /utf-8 /permissive-)
        target_include_directories(candidate-ui-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        llavon_target_winui3(candidate-ui-tests NO_DEPLOY)
        target_sources(candidate-ui-tests PRIVATE src/ui_resources.rc)
        target_compile_options(candidate-ui-tests PRIVATE /FI"${CMAKE_CURRENT_SOURCE_DIR}/src/ui/winui_platform.hpp")
        target_link_libraries(candidate-ui-tests PRIVATE llavon-ime-ui gdiplus ole32 oleaut32 uiautomationcore)
        add_test(NAME candidate-ui-candidate-first COMMAND candidate-ui-tests candidate-first)
        add_test(NAME candidate-ui-settings-first COMMAND candidate-ui-tests settings-first)
        add_test(NAME settings-ui-first-show COMMAND candidate-ui-tests settings-window-first)
        add_test(NAME settings-ui-hidden-startup COMMAND candidate-ui-tests settings-window-hidden)
        add_test(NAME settings-ui-menu-hidden-startup COMMAND candidate-ui-tests settings-menu-hidden)
        add_test(NAME settings-ui-password COMMAND candidate-ui-tests settings-password)
        set_tests_properties(candidate-ui-candidate-first candidate-ui-settings-first
            settings-ui-first-show settings-ui-hidden-startup settings-ui-menu-hidden-startup settings-ui-password PROPERTIES
            LABELS "desktop-ui" RUN_SERIAL TRUE TIMEOUT 60
            WORKING_DIRECTORY "$<TARGET_FILE_DIR:candidate-ui-tests>")
        set_tests_properties(settings-ui-password PROPERTIES TIMEOUT 180)
    endif()

    add_executable(lora-process-tests
        tests/lora_process_tests.cpp
        src/service/lora_process.cpp
    )
    target_compile_features(lora-process-tests PRIVATE cxx_std_23)
    target_compile_definitions(lora-process-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(lora-process-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(lora-process-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME lora-process COMMAND lora-process-tests)
    set_tests_properties(lora-process PROPERTIES TIMEOUT 30)

    add_executable(model-revision-cleanup-tests
        tests/model_revision_cleanup_tests.cpp
        src/service/model_revision_cleanup.cpp
        src/service/model_revision_cleanup.hpp
    )
    target_compile_features(model-revision-cleanup-tests PRIVATE cxx_std_23)
    target_compile_definitions(model-revision-cleanup-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE
        WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(model-revision-cleanup-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(model-revision-cleanup-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME model-revision-cleanup COMMAND model-revision-cleanup-tests)
    set_tests_properties(model-revision-cleanup PROPERTIES TIMEOUT 30)

    add_executable(lora-history-lca-tests
        tests/lora_history_lca_tests.cpp
        src/settings/lora_history_lca.cpp
        src/settings/lora_history_lca.hpp
    )
    target_compile_features(lora-history-lca-tests PRIVATE cxx_std_23)
    target_compile_options(lora-history-lca-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(lora-history-lca-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    add_test(NAME lora-history-lca COMMAND lora-history-lca-tests)

    add_executable(gpu-activity-lease-tests tests/gpu_activity_lease_tests.cpp)
    target_compile_features(gpu-activity-lease-tests PRIVATE cxx_std_23)
    target_compile_definitions(gpu-activity-lease-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(gpu-activity-lease-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(gpu-activity-lease-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(gpu-activity-lease-tests PRIVATE asio::asio ws2_32)
    add_test(NAME gpu-activity-lease COMMAND gpu-activity-lease-tests)

    add_executable(amd-gpu-boost-worker-tests
        tests/amd_gpu_boost_worker_tests.cpp
        src/service/amd_gpu_boost_worker.cpp)
    target_compile_features(amd-gpu-boost-worker-tests PRIVATE cxx_std_23)
    target_compile_definitions(amd-gpu-boost-worker-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_compile_options(amd-gpu-boost-worker-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(amd-gpu-boost-worker-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(amd-gpu-boost-worker-tests PRIVATE asio::asio ws2_32)
    add_test(NAME amd-gpu-boost-worker COMMAND amd-gpu-boost-worker-tests)
    set_tests_properties(amd-gpu-boost-worker PROPERTIES TIMEOUT 45)

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
        src/service/base_model_fingerprint.cpp
        src/service/base_model_fingerprint.hpp
        src/service/lora_dataset_builder.cpp
        src/service/lora_dataset_builder.hpp
        src/service/training_data_writer.cpp
        src/service/training_data_writer.hpp
    )
    target_compile_features(training-data-writer-tests PRIVATE cxx_std_23)
    target_compile_definitions(training-data-writer-tests PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        LLAVON_TEST_TABLES_DIR=L"${CMAKE_SOURCE_DIR}/ime-core/table"
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
        ime-core::ime-core
        utf8::cpp
        utf8cpp::utf8cpp
        reflectcpp::reflectcpp
        unofficial-sodium::sodium
        unofficial::sqlite3::sqlite3
        ole32
        bcrypt
        shell32
    )
    add_test(NAME training-data-writer COMMAND training-data-writer-tests)

    add_executable(winrt-http-tests
        tests/winrt_http_tests.cpp
        src/service/winrt_http.cpp
        src/service/winrt_http.hpp
        src/service/process_mta.hpp
    )
    target_compile_features(winrt-http-tests PRIVATE cxx_std_23)
    target_compile_definitions(winrt-http-tests PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX WINVER=0x0A00 _WIN32_WINNT=0x0A00)
    target_compile_options(winrt-http-tests PRIVATE /W4 /WX /utf-8 /permissive-)
    target_include_directories(winrt-http-tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(winrt-http-tests PRIVATE
        asio::asio Microsoft::CppWinRT WindowsApp ws2_32)
    add_test(NAME winrt-http COMMAND winrt-http-tests)
    set_tests_properties(winrt-http PROPERTIES TIMEOUT 20)

endif()

install(TARGETS
    llavon-ime-service
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)
