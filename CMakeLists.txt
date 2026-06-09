cmake_minimum_required(VERSION 3.20)
project(qwen3_tts_workspace LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_subdirectory(engine)
add_subdirectory(apps/streaming_cli)
