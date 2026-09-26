include("${CMAKE_CURRENT_LIST_DIR}/x64-windows.cmake")

# Build every supported x64 CPU backend as a loadable module. At runtime ggml
# scores the modules against the current CPU and loads the fastest compatible
# one, so the package is not tied to the GitHub runner's instruction set.
if(PORT STREQUAL "ggml")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DBUILD_SHARED_LIBS=ON"
        "-DGGML_NATIVE=OFF"
        "-DGGML_BACKEND_DL=ON"
        "-DGGML_CPU_ALL_VARIANTS=ON"
    )
endif()
