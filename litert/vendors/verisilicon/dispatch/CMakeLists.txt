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

# VeriSilicon Dispatch API Shared Library
# This .so runs on the target device (Pi). It does late-binding dlopen()
# of libvip-lite.so at runtime, so we need NO vendor NPU libraries at link time.
cmake_minimum_required(VERSION 3.20)

add_library(dispatch_api_verisilicon_so SHARED
    dispatch_api.cc
    viplite_adapter_api.cc
    litert_dispatch_device_context.cc
    litert_dispatch_invocation_context.cc
)

target_include_directories(dispatch_api_verisilicon_so
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)

# ViPLite adapter does late binding (dlopen) so we link against the
# LiteRT runtime C API static library — the dispatch .so is self-contained
# and loads libvip-lite.so from the device at runtime.
target_link_libraries(dispatch_api_verisilicon_so
    PRIVATE
        litert_runtime_c_api_static
        absl::flat_hash_set
        absl::str_format
        absl::strings
        absl::string_view
        absl::span
        absl::status
        absl::statusor
)

if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    target_link_libraries(dispatch_api_verisilicon_so PRIVATE android log)
endif()

set_target_properties(dispatch_api_verisilicon_so PROPERTIES
    OUTPUT_NAME "LiteRtDispatch_Verisilicon"
)

if(UNIX AND NOT APPLE)
    target_link_options(dispatch_api_verisilicon_so PRIVATE
        "-Wl,-soname=libLiteRtDispatch_Verisilicon.so"
    )
    # Match Samsung's pattern: link the full LiteRT runtime stack so all
    # symbols (including GpuEnvironment) are resolved at link time.
    target_link_libraries(dispatch_api_verisilicon_so PRIVATE
        "-Wl,--start-group"
        litert_runtime_c_api_shared_lib
        litert_cc_api
        litert_cc_options
        tensorflow-lite
        Threads::Threads
        "-Wl,--end-group"
    )
endif()
