add_library(qwen3_tts_streaming STATIC
    src/streaming/Qwen3StreamingTts.cpp
)
add_library(qwen3_tts::streaming ALIAS qwen3_tts_streaming)

target_include_directories(qwen3_tts_streaming PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(qwen3_tts_streaming PUBLIC tts_engine)

if (MSVC)
    target_compile_options(qwen3_tts_streaming PRIVATE /W3)
else()
    target_compile_options(qwen3_tts_streaming PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(QWEN3_TTS_BUILD_STREAMING_CLI)
    add_executable(qwen3_streaming_cli src/cli/main.cpp)
    target_link_libraries(qwen3_streaming_cli PRIVATE qwen3_tts_streaming)
    if (MSVC)
        target_compile_options(qwen3_streaming_cli PRIVATE /W3)
    else()
        target_compile_options(qwen3_streaming_cli PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(WIN32 AND QWEN3_TTS_EMBED_GGML)
        add_custom_command(TARGET qwen3_streaming_cli POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_BINARY_DIR}/bin
                $<TARGET_FILE_DIR:qwen3_streaming_cli>
        )
    endif()
endif()

if(QWEN3_TTS_BUILD_STREAMING_WRAPPER)
    install(TARGETS qwen3_tts_streaming ARCHIVE DESTINATION lib)
    install(FILES include/offgrid_tts/Qwen3StreamingTts.h DESTINATION include/offgrid_tts)
endif()
