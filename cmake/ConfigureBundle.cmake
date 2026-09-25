foreach(_required IN ITEMS
    LLAVON_IME_MODEL_REPO
    LLAVON_IME_MODEL_FILENAME
    LLAVON_IME_MODEL_INSTALLER_PATH
    LLAVON_IME_MSI_PATH
    LLAVON_IME_VERSION
    LLAVON_IME_SOURCE_DIR
    LLAVON_IME_BUNDLE_TEMPLATE
    LLAVON_IME_BUNDLE_OUTPUT)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

if(NOT LLAVON_IME_MODEL_REVISION OR NOT LLAVON_IME_MODEL_SHA256 OR
   NOT LLAVON_IME_MODEL_SIZE)
    if(LLAVON_IME_MODEL_REVISION)
        set(_metadata_revision "${LLAVON_IME_MODEL_REVISION}")
    else()
        set(_metadata_revision "main")
    endif()
    set(_metadata_url
        "https://huggingface.co/api/models/${LLAVON_IME_MODEL_REPO}/revision/${_metadata_revision}?blobs=true")
    set(_metadata_file "${LLAVON_IME_BUNDLE_OUTPUT}.metadata.json")
    file(DOWNLOAD "${_metadata_url}" "${_metadata_file}"
        STATUS _status TLS_VERIFY ON)
    list(GET _status 0 _code)
    if(NOT _code EQUAL 0)
        file(REMOVE "${_metadata_file}")
        message(FATAL_ERROR "Unable to resolve model metadata: ${_status}")
    endif()
    file(READ "${_metadata_file}" _metadata)
    file(REMOVE "${_metadata_file}")
    string(JSON _resolved_revision ERROR_VARIABLE _error GET "${_metadata}" sha)
    if(_error)
        message(FATAL_ERROR "Model metadata is missing its revision")
    endif()
    if(LLAVON_IME_MODEL_REVISION AND
       NOT LLAVON_IME_MODEL_REVISION STREQUAL _resolved_revision)
        message(FATAL_ERROR "Model metadata revision does not match the pinned revision")
    endif()
    set(LLAVON_IME_MODEL_REVISION "${_resolved_revision}")
    string(JSON _count ERROR_VARIABLE _error LENGTH "${_metadata}" siblings)
    if(_error)
        message(FATAL_ERROR "Model metadata has no files")
    endif()
    if(_count EQUAL 0)
        message(FATAL_ERROR "Model metadata has no files")
    endif()
    math(EXPR _last "${_count} - 1")
    set(_matches 0)
    foreach(_index RANGE 0 ${_last})
        string(JSON _name GET "${_metadata}" siblings ${_index} rfilename)
        if(_name STREQUAL LLAVON_IME_MODEL_FILENAME)
            math(EXPR _matches "${_matches} + 1")
            string(JSON LLAVON_IME_MODEL_SHA256 ERROR_VARIABLE _error
                GET "${_metadata}" siblings ${_index} lfs sha256)
            if(_error)
                message(FATAL_ERROR "Model metadata has no LFS SHA-256")
            endif()
            string(JSON LLAVON_IME_MODEL_SIZE ERROR_VARIABLE _error
                GET "${_metadata}" siblings ${_index} lfs size)
            if(_error)
                message(FATAL_ERROR "Model metadata has no LFS size")
            endif()
        endif()
    endforeach()
    if(NOT _matches EQUAL 1)
        message(FATAL_ERROR "Expected one model file, found ${_matches}")
    endif()
endif()

string(LENGTH "${LLAVON_IME_MODEL_REVISION}" _revision_length)
string(LENGTH "${LLAVON_IME_MODEL_SHA256}" _sha_length)
if(NOT LLAVON_IME_MODEL_REVISION MATCHES "^[0-9a-fA-F]+$" OR
   NOT _revision_length EQUAL 40 OR
   NOT LLAVON_IME_MODEL_SHA256 MATCHES "^[0-9a-fA-F]+$" OR
   NOT _sha_length EQUAL 64 OR
   NOT LLAVON_IME_MODEL_SIZE MATCHES "^[0-9]+$" OR
   LLAVON_IME_MODEL_SIZE EQUAL 0)
    message(FATAL_ERROR "Invalid model revision, SHA-256, or size")
endif()

configure_file("${LLAVON_IME_BUNDLE_TEMPLATE}"
    "${LLAVON_IME_BUNDLE_OUTPUT}" @ONLY)
