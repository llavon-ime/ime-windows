add_library(llavon-ipc INTERFACE)
add_library(llavon::ipc ALIAS llavon-ipc)

target_compile_features(llavon-ipc INTERFACE cxx_std_23)
target_include_directories(llavon-ipc INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(llavon-ipc INTERFACE advapi32)
