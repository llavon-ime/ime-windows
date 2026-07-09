if(NOT DEFINED LLAVON_IME_MODEL_URL OR LLAVON_IME_MODEL_URL STREQUAL "")
    message(FATAL_ERROR "LLAVON_IME_MODEL_URL is required")
endif()

if(NOT DEFINED LLAVON_IME_MODEL_PATH OR LLAVON_IME_MODEL_PATH STREQUAL "")
    message(FATAL_ERROR "LLAVON_IME_MODEL_PATH is required")
endif()

get_filename_component(_model_dir "${LLAVON_IME_MODEL_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${_model_dir}")

if(EXISTS "${LLAVON_IME_MODEL_PATH}")
    message(STATUS "Model already exists: ${LLAVON_IME_MODEL_PATH}")
    return()
endif()

message(STATUS "Downloading model from ${LLAVON_IME_MODEL_URL}")
file(DOWNLOAD
    "${LLAVON_IME_MODEL_URL}"
    "${LLAVON_IME_MODEL_PATH}"
    SHOW_PROGRESS
    STATUS _download_status
    TLS_VERIFY ON
)

list(GET _download_status 0 _download_code)
list(GET _download_status 1 _download_message)

if(NOT _download_code EQUAL 0)
    file(REMOVE "${LLAVON_IME_MODEL_PATH}")
    message(FATAL_ERROR "Model download failed: ${_download_message}")
endif()
