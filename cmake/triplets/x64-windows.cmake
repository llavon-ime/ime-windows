set(VCPKG_TARGET_ARCHITECTURE x64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE dynamic)

# TSF loads the frontend inside AppContainer hosts, whose dependency search
# does not include the IME installation directory. reflectcpp is already static
# on Windows; its JSON backend must also be linked into the frontend DLL.
if(PORT STREQUAL "yyjson")
    set(VCPKG_LIBRARY_LINKAGE static)
endif()
