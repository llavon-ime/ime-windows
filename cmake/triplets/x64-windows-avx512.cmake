set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# Keep an explicitly AVX-512-optimized package without depending on the
# instruction set exposed by whichever GitHub-hosted runner handles the job.
if(PORT STREQUAL "ggml")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DGGML_NATIVE=OFF"
        "-DGGML_SSE42=ON"
        "-DGGML_AVX=ON"
        "-DGGML_AVX2=ON"
        "-DGGML_BMI2=ON"
        "-DGGML_AVX_VNNI=OFF"
        "-DGGML_AVX512=ON"
        "-DGGML_AVX512_VBMI=OFF"
        "-DGGML_AVX512_VNNI=OFF"
        "-DGGML_AVX512_BF16=OFF"
    )
endif()
