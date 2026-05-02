# Install script for directory: C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml

# Set the install prefix
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  set(CMAKE_INSTALL_PREFIX "C:/Program Files (x86)/qwen3-tts-ggml")
endif()
string(REGEX REPLACE "/$" "" CMAKE_INSTALL_PREFIX "${CMAKE_INSTALL_PREFIX}")

# Set the install configuration name.
if(NOT DEFINED CMAKE_INSTALL_CONFIG_NAME)
  if(BUILD_TYPE)
    string(REGEX REPLACE "^[^A-Za-z0-9_]+" ""
           CMAKE_INSTALL_CONFIG_NAME "${BUILD_TYPE}")
  else()
    set(CMAKE_INSTALL_CONFIG_NAME "Release")
  endif()
  message(STATUS "Install configuration: \"${CMAKE_INSTALL_CONFIG_NAME}\"")
endif()

# Set the component getting installed.
if(NOT CMAKE_INSTALL_COMPONENT)
  if(COMPONENT)
    message(STATUS "Install component: \"${COMPONENT}\"")
    set(CMAKE_INSTALL_COMPONENT "${COMPONENT}")
  else()
    set(CMAKE_INSTALL_COMPONENT)
  endif()
endif()

# Is this installation the result of a crosscompile?
if(NOT DEFINED CMAKE_CROSSCOMPILING)
  set(CMAKE_CROSSCOMPILING "FALSE")
endif()

if(NOT CMAKE_INSTALL_LOCAL_ONLY)
  # Include the install script for the subdirectory.
  include("C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/ggml/src/cmake_install.cmake")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/ggml/src/ggml.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/bin/ggml.dll")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/include" TYPE FILE FILES
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-cpu.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-alloc.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-backend.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-blas.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-cann.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-cpp.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-cuda.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-opt.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-metal.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-rpc.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-virtgpu.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-sycl.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-vulkan.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-webgpu.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/ggml-zendnn.h"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/ggml/include/gguf.h"
    )
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib" TYPE STATIC_LIBRARY OPTIONAL FILES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/ggml/src/ggml-base.lib")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/bin" TYPE SHARED_LIBRARY FILES "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/bin/ggml-base.dll")
endif()

if(CMAKE_INSTALL_COMPONENT STREQUAL "Unspecified" OR NOT CMAKE_INSTALL_COMPONENT)
  file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/cmake/ggml" TYPE FILE FILES
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/ggml/ggml-config.cmake"
    "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/ggml/ggml-version.cmake"
    )
endif()

string(REPLACE ";" "\n" CMAKE_INSTALL_MANIFEST_CONTENT
       "${CMAKE_INSTALL_MANIFEST_FILES}")
if(CMAKE_INSTALL_LOCAL_ONLY)
  file(WRITE "C:/git/qwen3-tts-cpp-streaming/third_party/qwen3-tts-cpp/build/ggml/install_local_manifest.txt"
     "${CMAKE_INSTALL_MANIFEST_CONTENT}")
endif()
