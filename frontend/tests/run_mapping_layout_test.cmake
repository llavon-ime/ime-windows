# Model deployment without inheriting the developer's table override or CWD.
set(_root "${TEST_ROOT}/frontend layout")
set(_tables "${_root}/tables")
if(LAYOUT STREQUAL "installed-x64")
    set(_bin "${_root}/bin")
elseif(LAYOUT STREQUAL "installed-x86")
    set(_bin "${_root}/bin/x86")
elseif(LAYOUT STREQUAL "development-x64")
    set(_bin "${_root}/build/windows/bin")
    set(_tables "${_root}/ime-core/table")
elseif(LAYOUT STREQUAL "development-x86")
    set(_bin "${_root}/build/windows/frontend-x86/bin")
    set(_tables "${_root}/ime-core/table")
else()
    message(FATAL_ERROR "Unknown frontend layout: ${LAYOUT}")
endif()
file(MAKE_DIRECTORY "${_bin}" "${_tables}" "${_root}/unrelated-working-directory")
file(COPY_FILE "${TEST_EXECUTABLE}" "${_bin}/mapping-layout-test.exe" ONLY_IF_DIFFERENT)
file(COPY_FILE "${MAPPING_FILE}" "${_tables}/bopomofo_char.json" ONLY_IF_DIFFERENT)
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env --unset=LLAVON_IME_TABLES_DIR
        "${_bin}/mapping-layout-test.exe"
    WORKING_DIRECTORY "${_root}/unrelated-working-directory"
    COMMAND_ERROR_IS_FATAL ANY
    TIMEOUT 15
)
