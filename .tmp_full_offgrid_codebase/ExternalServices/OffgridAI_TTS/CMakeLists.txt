cmake_minimum_required(VERSION 3.20)
project(OffgridAI_TTS LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(OFFGRID_TTS_ENABLE_CUDA "Build ggml with CUDA backend for Qwen3 TTS" ON)
option(OFFGRID_TTS_ENABLE_CUDA_GRAPHS "Enable CUDA graph support in ggml when CUDA is enabled" ON)

set(QWEN3_STREAMING_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qwen3-tts-cpp-streaming")
set(QWEN3_ENGINE_DIR "${QWEN3_STREAMING_DIR}/engine")
set(QWEN3_GGML_DIR "${QWEN3_ENGINE_DIR}/ggml")

if (NOT EXISTS "${QWEN3_ENGINE_DIR}/src/qwen3_tts.h")
    message(FATAL_ERROR "Missing qwen3-tts-cpp-streaming engine source at ${QWEN3_ENGINE_DIR}")
endif()
if (NOT EXISTS "${QWEN3_GGML_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Missing ggml source at ${QWEN3_GGML_DIR}. Populate ggml under qwen3-tts-cpp-streaming/engine/ggml.")
endif()

message(STATUS "OffgridAI_TTS: clean VoiceDesign service layout")
message(STATUS "OffgridAI_TTS qwen source: ${QWEN3_STREAMING_DIR}")
message(STATUS "OffgridAI_TTS CUDA requested: ${OFFGRID_TTS_ENABLE_CUDA}")
message(STATUS "OffgridAI_TTS CUDA graphs requested: ${OFFGRID_TTS_ENABLE_CUDA_GRAPHS}")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(QWEN3_TTS_EMBED_GGML ON CACHE BOOL "" FORCE)
set(QWEN3_TTS_GGML_DIR "${QWEN3_GGML_DIR}" CACHE PATH "" FORCE)
set(QWEN3_TTS_CUDA ${OFFGRID_TTS_ENABLE_CUDA} CACHE BOOL "" FORCE)
set(GGML_STATIC ON CACHE BOOL "" FORCE)
set(GGML_NATIVE ON CACHE BOOL "" FORCE)
set(GGML_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GGML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GGML_BACKEND_DL OFF CACHE BOOL "" FORCE)
set(GGML_CPU ON CACHE BOOL "" FORCE)
set(GGML_CUDA ${OFFGRID_TTS_ENABLE_CUDA} CACHE BOOL "" FORCE)
set(GGML_CUDA_GRAPHS ${OFFGRID_TTS_ENABLE_CUDA_GRAPHS} CACHE BOOL "" FORCE)
set(GGML_CUDA_FORCE_CUBLAS ON CACHE BOOL "" FORCE)
set(GGML_CUDA_FA ON CACHE BOOL "" FORCE)

add_subdirectory("${QWEN3_ENGINE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/qwen3_engine" EXCLUDE_FROM_ALL)

if (OFFGRID_TTS_ENABLE_CUDA AND NOT TARGET ggml-cuda)
    message(FATAL_ERROR "OFFGRID_TTS_ENABLE_CUDA=ON but ggml-cuda target was not created. Check CUDA Toolkit discovery.")
endif()

add_executable(OffgridAI_TTS
    src/main.cpp
    src/App/TTSServiceApp.cpp
    src/Transport/TTSPipeServer.cpp
    src/Protocol/TTSProtocol.cpp
    src/Backend/Qwen3TTSBackend.cpp
)

target_include_directories(OffgridAI_TTS PRIVATE
    src
    ${QWEN3_ENGINE_DIR}/src
    ${QWEN3_GGML_DIR}/include
)

target_link_libraries(OffgridAI_TTS PRIVATE tts_engine)

if (OFFGRID_TTS_ENABLE_CUDA)
    target_compile_definitions(OffgridAI_TTS PRIVATE OFFGRID_TTS_BUILT_WITH_CUDA=1 GGML_USE_CUDA=1)
else()
    target_compile_definitions(OffgridAI_TTS PRIVATE OFFGRID_TTS_BUILT_WITH_CUDA=0)
endif()

if (WIN32)
    target_compile_definitions(OffgridAI_TTS PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(OffgridAI_TTS PRIVATE winmm)
endif()

if (MSVC)
    target_compile_options(OffgridAI_TTS PRIVATE /W3 /EHsc /bigobj)
else()
    target_compile_options(OffgridAI_TTS PRIVATE -Wall -Wextra)
endif()
