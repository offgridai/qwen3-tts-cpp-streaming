cmake_minimum_required(VERSION 3.14)
project(tts_engine VERSION 0.1.0 LANGUAGES CXX)

# C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Compiler flags
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -Wpedantic")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O3 -march=native")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -g -O0")
endif()

# Timing instrumentation option
option(QWEN3_TTS_TIMING "Enable detailed timing instrumentation" OFF)
if(QWEN3_TTS_TIMING)
    add_compile_definitions(QWEN3_TTS_TIMING)
endif()

# CoreML code predictor bridge (macOS only)
option(QWEN3_TTS_COREML "Enable CoreML code predictor bridge on macOS" ON)
if(APPLE AND QWEN3_TTS_COREML)
    enable_language(OBJCXX)
endif()

# Find threads
find_package(Threads REQUIRED)

# GGML location/build mode
set(QWEN3_TTS_GGML_DIR "${CMAKE_CURRENT_SOURCE_DIR}/ggml" CACHE PATH "Path to GGML source directory")
option(QWEN3_TTS_EMBED_GGML "Build GGML as a CMake subdirectory instead of linking prebuilt libs" ON)
set(QWEN3_TTS_GGML_BUILD_DIR "${QWEN3_TTS_GGML_DIR}/build" CACHE PATH "Path to prebuilt GGML build directory (used when QWEN3_TTS_EMBED_GGML=OFF)")

option(QWEN3_TTS_CUDA "Enable CUDA backend via GGML" OFF)
set(QWEN3_TTS_CUDA_ARCHITECTURES "portable" CACHE STRING
    "CUDA targets: portable, turing, native, or a CMake architecture list such as 75-real;89;120a-real")
set_property(CACHE QWEN3_TTS_CUDA_ARCHITECTURES PROPERTY STRINGS portable turing native)
if(QWEN3_TTS_CUDA)
    set(GGML_CUDA ON CACHE BOOL "Enable GGML CUDA backend" FORCE)

    # GGML otherwise defaults to the GPU present at build time. That produces a
    # single-architecture DLL which cannot be redistributed across Turing GTX
    # 16-series, Ada RTX 40-series, and Blackwell RTX 50-series machines. Keep an
    # explicit native mode for quick local builds, but make normal builds portable.
    if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
        find_package(CUDAToolkit REQUIRED)

        if(QWEN3_TTS_CUDA_ARCHITECTURES STREQUAL "portable")
            if(CUDAToolkit_VERSION VERSION_LESS "11.8")
                message(FATAL_ERROR
                    "The portable CUDA build requires CUDA 11.8 or newer for RTX 4090 (sm_89) support.")
            endif()

            # Ship native Turing code for GTX 16-series cards. Ada includes PTX
            # for forward compatibility; Blackwell's target requires CUDA 12.8.
            set(_qwen3_cuda_architectures 75-real 89)
            if(CUDAToolkit_VERSION VERSION_GREATER_EQUAL "12.8")
                list(APPEND _qwen3_cuda_architectures 120a-real)
            endif()
            set(CMAKE_CUDA_ARCHITECTURES "${_qwen3_cuda_architectures}")
        elseif(QWEN3_TTS_CUDA_ARCHITECTURES STREQUAL "turing")
            # GTX 16-series Turing GPUs omit tensor cores. Force GGML's integer
            # MMQ kernels for quantized weights instead of its tensor-core path.
            set(CMAKE_CUDA_ARCHITECTURES 75)
            set(GGML_CUDA_FORCE_MMQ ON CACHE BOOL
                "Use MMQ kernels on Turing GPUs without tensor cores" FORCE)
        elseif(QWEN3_TTS_CUDA_ARCHITECTURES STREQUAL "native")
            set(CMAKE_CUDA_ARCHITECTURES native)
        elseif(QWEN3_TTS_CUDA_ARCHITECTURES)
            set(CMAKE_CUDA_ARCHITECTURES "${QWEN3_TTS_CUDA_ARCHITECTURES}")
        else()
            message(FATAL_ERROR
                "QWEN3_TTS_CUDA_ARCHITECTURES must be portable, turing, native, or a CMake architecture list.")
        endif()
    endif()

    message(STATUS "Qwen3-TTS CUDA architectures: ${CMAKE_CUDA_ARCHITECTURES}")
endif()

set(GGML_DIR "${QWEN3_TTS_GGML_DIR}")
set(GGML_BUILD_DIR "${QWEN3_TTS_GGML_BUILD_DIR}")

if(NOT EXISTS "${GGML_DIR}/include/ggml.h")
    message(FATAL_ERROR "GGML headers not found in '${GGML_DIR}'. Set -DQWEN3_TTS_GGML_DIR=<path-to-ggml>.")
endif()

if(QWEN3_TTS_EMBED_GGML)
    if(NOT EXISTS "${GGML_DIR}/CMakeLists.txt")
        message(FATAL_ERROR "GGML CMakeLists.txt not found in '${GGML_DIR}'.")
    endif()
    set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory("${GGML_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/ggml")
endif()

function(qwen3_link_ggml target_name)
    if(QWEN3_TTS_EMBED_GGML)
        target_link_libraries(${target_name} PUBLIC ggml ggml-base)
        if(QWEN3_TTS_CUDA)
            target_link_libraries(${target_name} PUBLIC ggml-cuda)
        else()
            target_link_libraries(${target_name} PUBLIC ggml-cpu)
        endif()
    else()
        target_link_directories(${target_name} PUBLIC ${GGML_BUILD_DIR}/src)
        target_link_libraries(${target_name} PUBLIC ggml ggml-base)
        if(QWEN3_TTS_CUDA)
            target_link_libraries(${target_name} PUBLIC ggml-cuda)
        else()
            target_link_libraries(${target_name} PUBLIC ggml-cpu)
        endif()
    endif()
endfunction()

# Text tokenizer library
add_library(text_tokenizer STATIC
    src/text_tokenizer.cpp
    src/gguf_loader.cpp
    src/tokenizer_unicode.cpp
    src/tokenizer_unicode_data.cpp
)
target_include_directories(text_tokenizer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GGML_DIR}/include
)
qwen3_link_ggml(text_tokenizer)
target_link_libraries(text_tokenizer PUBLIC Threads::Threads)

# TTS Transformer library (GGML-based + optional CoreML bridge)
set(TTS_TRANSFORMER_SOURCES
    src/gguf_loader.cpp
    src/tts_transformer.cpp
    src/transformer/transformer_cache.cpp
    src/transformer/transformer_debug.cpp
    src/transformer/transformer_embeddings.cpp
    src/transformer/transformer_graph_code_pred.cpp
    src/transformer/transformer_graph_talker.cpp
    src/transformer/transformer_loader.cpp
    src/transformer/transformer_loader_metadata.cpp
    src/transformer/transformer_loader_tensors.cpp
    src/transformer/transformer_runtime.cpp
    src/transformer/transformer_runtime_code_pred.cpp
    src/transformer/transformer_generate.cpp
)
if(APPLE AND QWEN3_TTS_COREML)
    list(APPEND TTS_TRANSFORMER_SOURCES src/coreml_code_predictor.mm)
    set_source_files_properties(src/coreml_code_predictor.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
else()
    list(APPEND TTS_TRANSFORMER_SOURCES src/coreml_code_predictor_stub.cpp)
endif()

add_library(tts_transformer STATIC
    ${TTS_TRANSFORMER_SOURCES}
)
target_include_directories(tts_transformer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GGML_DIR}/include
)
qwen3_link_ggml(tts_transformer)
target_link_libraries(tts_transformer PUBLIC Threads::Threads)
if(APPLE AND QWEN3_TTS_COREML)
    target_link_libraries(tts_transformer PUBLIC
        "-framework Foundation"
        "-framework CoreML"
    )
endif()

# Audio tokenizer encoder library (GGML-based)
add_library(audio_tokenizer_encoder STATIC
    src/audio_tokenizer_encoder.cpp
    src/encoder/encoder_frontend.cpp
    src/encoder/encoder_graph.cpp
    src/encoder/encoder_loader.cpp
    src/encoder/encoder_runtime.cpp
    src/gguf_loader.cpp
)
target_include_directories(audio_tokenizer_encoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GGML_DIR}/include
)
qwen3_link_ggml(audio_tokenizer_encoder)
target_link_libraries(audio_tokenizer_encoder PUBLIC Threads::Threads)

# Audio tokenizer decoder library (GGML-based)
add_library(audio_tokenizer_decoder STATIC
    src/decoder/decoder_cache.cpp
    src/decoder/decoder_graph.cpp
    src/decoder/decoder_layers.cpp
    src/decoder/decoder_loader.cpp
    src/decoder/decoder_runtime.cpp
    src/gguf_loader.cpp
)
target_include_directories(audio_tokenizer_decoder PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GGML_DIR}/include
)
qwen3_link_ggml(audio_tokenizer_decoder)
target_link_libraries(audio_tokenizer_decoder PUBLIC Threads::Threads)

# Qwen3 TTS library (full pipeline)
add_library(tts_engine STATIC
    src/common/audio_io.cpp
    src/common/speaker_embedding_io.cpp
    src/pipeline/pipeline_models.cpp
    src/pipeline/pipeline_runtime.cpp
    src/pipeline/pipeline_synthesize.cpp
    src/qwen3_tts.cpp
)
target_include_directories(tts_engine PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${GGML_DIR}/include
)
target_link_libraries(tts_engine PUBLIC
    text_tokenizer
    tts_transformer
    audio_tokenizer_encoder
    audio_tokenizer_decoder
    Threads::Threads
)
target_compile_definitions(tts_engine PRIVATE QWEN3_TTS_EXPORT)
if(WIN32)
    target_link_libraries(tts_engine PUBLIC Psapi)
endif()
add_library(qwen3_tts ALIAS tts_engine)

# CLI executable
add_executable(tts_engine_cli
    src/main.cpp
)
target_link_libraries(tts_engine_cli PRIVATE
    tts_engine
)

# Quantization tool
add_executable(tts_engine_quantize
    src/qwen3_tts_quantize.cpp
)
target_include_directories(tts_engine_quantize PRIVATE
    ${GGML_DIR}/include
)
qwen3_link_ggml(tts_engine_quantize)

if(WIN32 AND QWEN3_TTS_EMBED_GGML)
    foreach(engine_executable tts_engine_cli tts_engine_quantize)
        add_custom_command(TARGET ${engine_executable} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_BINARY_DIR}/bin
                $<TARGET_FILE_DIR:${engine_executable}>
        )
    endforeach()
endif()

# Install targets
install(TARGETS text_tokenizer tts_transformer audio_tokenizer_encoder audio_tokenizer_decoder tts_engine tts_engine_cli tts_engine_quantize
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES 
    src/gguf_loader.h 
    src/text_tokenizer.h 
    src/tts_transformer.h 
    src/coreml_code_predictor.h
    src/audio_tokenizer_encoder.h 
    src/audio_tokenizer_decoder.h 
    src/qwen3_tts.h
    DESTINATION include
)
