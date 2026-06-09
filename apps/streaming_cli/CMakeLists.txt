add_executable(qwen3_streaming_cli
    src/cli/main.cpp
    src/streaming/Qwen3StreamingTts.cpp
)

target_include_directories(qwen3_streaming_cli PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(qwen3_streaming_cli PRIVATE
    tts_engine
)

if (MSVC)
    target_compile_options(qwen3_streaming_cli PRIVATE /W3)
else()
    target_compile_options(qwen3_streaming_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
