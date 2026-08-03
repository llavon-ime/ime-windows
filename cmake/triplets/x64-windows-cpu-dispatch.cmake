set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

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
