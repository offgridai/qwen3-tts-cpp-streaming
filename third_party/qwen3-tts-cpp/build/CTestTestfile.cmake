# CMake generated Testfile for 
# Source directory: C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp
# Build directory: C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(tokenizer_test "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/test_tokenizer.exe")
set_tests_properties(tokenizer_test PROPERTIES  WORKING_DIRECTORY "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp" _BACKTRACE_TRIPLES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;269;add_test;C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;0;")
add_test(encoder_test "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/test_encoder.exe" "--tokenizer" "models/qwen3-tts-0.6b-f16.gguf" "--audio" "clone.wav" "--reference" "reference/ref_audio_embedding.bin")
set_tests_properties(encoder_test PROPERTIES  WORKING_DIRECTORY "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp" _BACKTRACE_TRIPLES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;273;add_test;C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;0;")
add_test(transformer_test "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/test_transformer.exe")
set_tests_properties(transformer_test PROPERTIES  WORKING_DIRECTORY "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp" _BACKTRACE_TRIPLES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;277;add_test;C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;0;")
add_test(decoder_test "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/test_decoder.exe")
set_tests_properties(decoder_test PROPERTIES  WORKING_DIRECTORY "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp" _BACKTRACE_TRIPLES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;281;add_test;C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/CMakeLists.txt;0;")
subdirs("ggml")
