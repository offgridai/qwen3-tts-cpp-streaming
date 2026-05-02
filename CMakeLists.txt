cmake_minimum_required(VERSION 3.20)
project(qwen3_tts_cpp_streaming LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(qwen3_streaming_cli
    src/cli/main.cpp
    src/streaming/Qwen3StreamingTts.cpp
)

target_include_directories(qwen3_streaming_cli PRIVATE
    include
)

if (MSVC)
    target_compile_options(qwen3_streaming_cli PRIVATE /W3)
else()
    target_compile_options(qwen3_streaming_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
