# Copyright 2025 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# VeriSilicon Compiler Plugin Shared Library
# This .so runs on the HOST (x86_64 in Docker) during AOT compilation.
# It shells out to pegasus (VeriSilicon's NBG compiler) to convert
# tflite partitions into NBG byte code, then embeds the NBG into the
# dispatch tflite model.
cmake_minimum_required(VERSION 3.20)

message(STATUS "Building VeriSilicon compiler plugin for host")

add_library(verisilicon_compiler_plugin SHARED
    compiler_plugin.cc
)

target_include_directories(verisilicon_compiler_plugin
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(verisilicon_compiler_plugin
    PRIVATE
        tensorflow-lite
        absl::log
        absl::check
        absl::strings
        absl::string_view
        absl::span
)

set_target_properties(verisilicon_compiler_plugin PROPERTIES
    OUTPUT_NAME "LiteRtCompilerPluginVerisilicon"
)

if(APPLE)
    target_link_libraries(verisilicon_compiler_plugin PRIVATE
        litert_runtime_c_api_shared_lib
        tensorflow-lite
        Threads::Threads
        litert_compiler_plugin
        litert_cc_api
        litert_cc_options
    )
elseif(UNIX)
    target_link_options(verisilicon_compiler_plugin PRIVATE
        "-Wl,-soname=libLiteRtCompilerPluginVerisilicon.so"
    )

    target_link_libraries(verisilicon_compiler_plugin PRIVATE
        "-Wl,--start-group"
        litert_runtime_c_api_shared_lib
        tensorflow-lite
        Threads::Threads
        litert_compiler_plugin
        litert_cc_api
        litert_cc_options
        "-Wl,--end-group"
    )
endif()
