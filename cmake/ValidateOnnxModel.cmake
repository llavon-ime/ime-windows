if(NOT DEFINED LLAVON_IME_ONNX_MODEL_DIR OR
   LLAVON_IME_ONNX_MODEL_DIR STREQUAL "")
    message(FATAL_ERROR "LLAVON_IME_ONNX_MODEL_DIR is required for an NPU-enabled package")
endif()

foreach(_file IN ITEMS
    genai_config.json
    model.onnx
    model.onnx.data
    tokenizer.json
    tokenizer_config.json)
    if(NOT EXISTS "${LLAVON_IME_ONNX_MODEL_DIR}/${_file}")
        message(FATAL_ERROR
            "The exported NPU model is incomplete: ${LLAVON_IME_ONNX_MODEL_DIR}/${_file}")
    endif()
endforeach()
