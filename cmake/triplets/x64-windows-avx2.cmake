set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# GitHub-hosted Windows runners may expose AVX-512. ggml otherwise defaults
# GGML_NATIVE to ON and produces a runner-specific binary that crashes with
# STATUS_ILLEGAL_INSTRUCTION on AVX2-only user machines.
if(PORT STREQUAL "ggml")
    list(APPEND VCPKG_CMAKE_CONFIGURE_OPTIONS
        "-DGGML_NATIVE=OFF"
        "-DGGML_SSE42=ON"
        "-DGGML_AVX=ON"
        "-DGGML_AVX2=ON"
        "-DGGML_BMI2=OFF"
        "-DGGML_AVX_VNNI=OFF"
        "-DGGML_AVX512=OFF"
        "-DGGML_AVX512_VBMI=OFF"
        "-DGGML_AVX512_VNNI=OFF"
        "-DGGML_AVX512_BF16=OFF"
    )
endif()
