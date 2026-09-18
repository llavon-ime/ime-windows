foreach(_required_variable IN ITEMS
        LLAVON_IME_MODEL_REPO
        LLAVON_IME_MODEL_FILENAME
        LLAVON_IME_MODEL_PATH)
    if(NOT DEFINED ${_required_variable} OR "${${_required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${_required_variable} is required")
    endif()
endforeach()

if(NOT DEFINED LLAVON_IME_MODEL_REVISION OR LLAVON_IME_MODEL_REVISION STREQUAL "")
    set(_metadata_url
        "https://huggingface.co/api/models/${LLAVON_IME_MODEL_REPO}/revision/main")
    set(_metadata_path "${LLAVON_IME_MODEL_PATH}.metadata.json")
    message(STATUS "Resolving the latest model revision from ${_metadata_url}")
    file(DOWNLOAD
        "${_metadata_url}"
        "${_metadata_path}"
        STATUS _metadata_status
        TLS_VERIFY ON
    )
    list(GET _metadata_status 0 _metadata_code)
    list(GET _metadata_status 1 _metadata_message)
    if(NOT _metadata_code EQUAL 0)
        file(REMOVE "${_metadata_path}")
        message(FATAL_ERROR "Model metadata download failed: ${_metadata_message}")
    endif()

    file(READ "${_metadata_path}" _metadata_json)
    file(REMOVE "${_metadata_path}")
    string(JSON LLAVON_IME_MODEL_REVISION ERROR_VARIABLE _revision_error
        GET "${_metadata_json}" sha)
    if(_revision_error)
        message(FATAL_ERROR "Hugging Face returned invalid model metadata")
    endif()
endif()

string(LENGTH "${LLAVON_IME_MODEL_REVISION}" _revision_length)
if(NOT LLAVON_IME_MODEL_REVISION MATCHES "^[0-9a-fA-F]+$" OR
   NOT _revision_length EQUAL 40)
    message(FATAL_ERROR "LLAVON_IME_MODEL_REVISION must be a 40-character commit SHA")
endif()

get_filename_component(_model_dir "${LLAVON_IME_MODEL_PATH}" DIRECTORY)
file(MAKE_DIRECTORY "${_model_dir}")
set(_revision_path "${LLAVON_IME_MODEL_PATH}.revision")

if(EXISTS "${LLAVON_IME_MODEL_PATH}" AND EXISTS "${_revision_path}")
    file(READ "${_revision_path}" _cached_revision)
    string(STRIP "${_cached_revision}" _cached_revision)
    if(_cached_revision STREQUAL LLAVON_IME_MODEL_REVISION)
        message(STATUS "Model is current at revision ${LLAVON_IME_MODEL_REVISION}")
        return()
    endif()
endif()

set(_model_url
    "https://huggingface.co/${LLAVON_IME_MODEL_REPO}/resolve/${LLAVON_IME_MODEL_REVISION}/${LLAVON_IME_MODEL_FILENAME}?download=true")
set(_partial_path "${LLAVON_IME_MODEL_PATH}.partial")
file(REMOVE "${_partial_path}")
message(STATUS "Downloading model revision ${LLAVON_IME_MODEL_REVISION}")
file(DOWNLOAD
    "${_model_url}"
    "${_partial_path}"
    SHOW_PROGRESS
    STATUS _download_status
    TLS_VERIFY ON
)
list(GET _download_status 0 _download_code)
list(GET _download_status 1 _download_message)
if(NOT _download_code EQUAL 0)
    file(REMOVE "${_partial_path}")
    message(FATAL_ERROR "Model download failed: ${_download_message}")
endif()

file(REMOVE "${LLAVON_IME_MODEL_PATH}")
file(RENAME "${_partial_path}" "${LLAVON_IME_MODEL_PATH}")
file(WRITE "${_revision_path}" "${LLAVON_IME_MODEL_REVISION}")
