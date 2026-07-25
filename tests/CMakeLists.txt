add_executable(qwen3_wrapper_contract_test
    wrapper_contract_test.cpp
)
target_link_libraries(qwen3_wrapper_contract_test PRIVATE qwen3_tts_streaming)

add_test(NAME wrapper_contract COMMAND qwen3_wrapper_contract_test)
set_tests_properties(wrapper_contract PROPERTIES LABELS "fast;contract")

add_executable(qwen3_wrapper_model_test
    wrapper_model_test.cpp
)
target_link_libraries(qwen3_wrapper_model_test PRIVATE qwen3_tts_streaming)

if(WIN32)
    foreach(test_target qwen3_wrapper_contract_test qwen3_wrapper_model_test)
        add_custom_command(TARGET ${test_target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${CMAKE_BINARY_DIR}/bin
                $<TARGET_FILE_DIR:${test_target}>
        )
    endforeach()
endif()

option(QWEN3_TTS_ENABLE_MODEL_TESTS "Register model-backed tests with CTest" OFF)
if(QWEN3_TTS_ENABLE_MODEL_TESTS)
    add_test(
        NAME wrapper_model_contract
        COMMAND qwen3_wrapper_model_test
            --models ${CMAKE_SOURCE_DIR}/models
            --model qwen3-tts-0.6b-f16
            --embedding ${CMAKE_SOURCE_DIR}/reference/priestley_0.6b_f16.json
            --output-dir ${CMAKE_BINARY_DIR}/acceptance
    )
    set_tests_properties(wrapper_model_contract PROPERTIES
        LABELS "model;gpu;contract"
        TIMEOUT 120
    )
endif()
